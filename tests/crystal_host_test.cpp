// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/crystal_host.h"
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string encode(const std::vector<std::string> &arguments)
{
    std::string result;
    for (const auto &arg : arguments)
        result += std::to_string(arg.size()) + ":" + arg + "\n";
    return result;
}
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--echo") {
#ifdef _WIN32
        // Text-mode CRLF conversion would obscure the argument byte contract
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::vector<std::string> arguments(argv + 2, argv + argc);
        std::cout << encode(arguments);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--fail") {
        std::cerr << "synthetic failure";
        return 23;
    }
    if (argc > 1 && std::string(argv[1]) == "--large") {
        std::cout << std::string(200000, 'x');
        return 0;
    }
    try {
#ifdef _WIN32
        require(GetACP() == CP_UTF8, "UTF-8 executable manifest missing");
#endif
        auto original = std::filesystem::absolute(
                std::filesystem::u8path(argv[0]));
        std::filesystem::path removed;
        {
            crystal_host::Temporary workspace;
            removed = workspace.directory;
            crystal_host::Temporary another;
            require(workspace.directory != another.directory,
                    "Temporary workspaces collided");
            auto folder = workspace.directory / u8"spaces \u96ea & percent %";
            std::filesystem::create_directory(folder);
            auto executable = folder / original.filename();
            std::filesystem::copy_file(original, executable);
            std::vector<std::string> arguments{
                "", "plain", "space here", "tab\there", "quote\"here",
                "\\", "space trailing\\", "\\\"", u8"\u96ea \u00e9",
                "& echo shell-must-not-run %PATH%", "a\nb"
            };
            auto invoke = [&] {
                auto child_args = arguments;
                child_args.insert(child_args.begin(), "--echo");
                require(crystal_host::run(executable.u8string(), child_args) ==
                        encode(arguments), "Child argv was altered");
            };
            invoke();
            // A slow or inherited output pipe would stall concurrent requests
            std::vector<std::future<void>> requests;
            for (int i = 0; i < 8; i++)
                requests.push_back(std::async(std::launch::async, invoke));
            for (auto &request : requests) request.get();
            require(crystal_host::run(executable.u8string(), { "--large" }) ==
                    std::string(65536, 'x'), "Output limit or drain failed");
            bool failed = false;
            try { crystal_host::run(executable.u8string(), { "--fail" }); }
            catch (const std::exception &error) {
                failed = std::string(error.what()).find("synthetic failure") !=
                         std::string::npos;
            }
            require(failed, "Helper failure lost diagnostics");
            failed = false;
            try { crystal_host::run((folder / "missing.exe").u8string(), {}); }
            catch (const std::exception &) { failed = true; }
            require(failed, "Missing helper was accepted");
            auto file = folder / u8"narrow \u96ea.txt";
            FILE *output = fopen(file.u8string().c_str(), "wb");
            require(output != nullptr, "Narrow UTF-8 file path failed");
            fputs("native path", output);
            fclose(output);
            require(std::filesystem::file_size(file) == 11,
                    "Narrow file API changed Unicode path");
#ifdef _WIN32
            require(std::filesystem::u8path(crystal_host::bundled_helper()) ==
                    original.parent_path() / "Bridge" / "crystal-bridge.exe",
                    "Bundled helper depends on the working directory");
#endif
        }
        require(!std::filesystem::exists(removed), "Workspace was not removed");
        try {
            crystal_host::Temporary workspace;
            removed = workspace.directory;
            std::ofstream(workspace.directory / "partial") << "partial";
            throw std::runtime_error("abort");
        } catch (const std::exception &) {}
        require(!std::filesystem::exists(removed),
                "Failed request left its workspace behind");
        if (argc == 3 && std::string(argv[1]) == "--bridge") {
            auto bridge = std::filesystem::absolute(
                    std::filesystem::u8path(argv[2]));
            auto output = crystal_host::run(bridge.u8string(), { "test" });
            require(output.find("unsupported-state checks passed") !=
                    std::string::npos, "Packaged helper process checks failed");
        }
        std::cout << "Crystal host process checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
