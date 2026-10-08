// SPDX-License-Identifier: GPL-3.0-or-later
#include "crystal_host.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace crystal_host {
namespace {
constexpr size_t MAX_OUTPUT = 65536;

void collect(std::string &output, const char *data, size_t length)
{
    // Continue draining after truncation so a verbose helper cannot deadlock
    output.append(data, std::min(length, MAX_OUTPUT - output.size()));
}

#ifdef _WIN32
std::runtime_error failure(const char *operation)
{
    return std::runtime_error(std::string(operation) + " (Windows error " +
                              std::to_string(GetLastError()) + ")");
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

std::wstring wide(const std::string &value)
{
    if (value.find('\0') != std::string::npos)
        throw std::runtime_error("Helper argument contains a null byte");
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     value.data(), value.size(), nullptr, 0);
    if (!length) throw failure("Invalid UTF-8 helper path or argument");
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                       value.size(), result.data(), length);
    return result;
}

std::wstring quoted(const std::string &value)
{
    // CreateProcess receives one command line; preserve CRT argument boundaries
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : wide(value)) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct Attributes {
    std::vector<unsigned char> storage;
    HANDLE handles[2];
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    Attributes(HANDLE output, HANDLE input)
        : handles{ output, input }
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage.resize(size);
        auto candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &size))
            throw failure("Cannot initialize helper handles");
        if (!UpdateProcThreadAttribute(candidate, 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles),
                nullptr, nullptr)) {
            DeleteProcThreadAttributeList(candidate);
            throw failure("Cannot isolate helper handles");
        }
        list = candidate;
    }
    ~Attributes() { DeleteProcThreadAttributeList(list); }
};
#endif
}

std::string run(const std::string &executable,
                const std::vector<std::string> &arguments)
{
    if (executable.empty())
        throw std::runtime_error(
                "Set --crystal-helper to the crystal-bridge executable");
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    Handle reader, writer, input;
    if (!CreatePipe(&reader.value, &writer.value, &security, 0) ||
        !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0))
        throw failure("Cannot open helper output pipe");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ |
            FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE)
        throw failure("Cannot open helper input");
    // Foreground and prefetch children must not inherit each other's pipes
    Attributes attributes(writer.value, input.value);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = writer.value;
    startup.StartupInfo.hStdError = writer.value;
    startup.lpAttributeList = attributes.list;
    std::wstring command = quoted(executable);
    for (const auto &argument : arguments) command += L" " + quoted(argument);
    auto application = wide(executable);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            nullptr, &startup.StartupInfo, &process))
        throw failure("Cannot start Crystal helper");
    Handle child{ process.hProcess }, thread{ process.hThread };
    CloseHandle(writer.value);
    writer.value = nullptr;
    std::string result;
    char buffer[2048];
    DWORD length = 0;
    DWORD read_error = ERROR_SUCCESS;
    while (true) {
        if (!ReadFile(reader.value, buffer, sizeof(buffer), &length, nullptr)) {
            read_error = GetLastError();
            break;
        }
        if (!length) break;
        collect(result, buffer, length);
    }
    if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0)
        throw failure("Cannot wait for Crystal helper");
    DWORD code = 0;
    if (!GetExitCodeProcess(child.value, &code))
        throw failure("Cannot read Crystal helper exit status");
    if (read_error != ERROR_SUCCESS && read_error != ERROR_BROKEN_PIPE)
        throw std::runtime_error("Cannot read helper output (Windows error " +
                                  std::to_string(read_error) + ")");
    if (code != 0)
        throw std::runtime_error("Crystal helper exited " +
                std::to_string(code) + ": " + result);
    return result;
#else
    int output[2];
    if (pipe(output))
        throw std::runtime_error("Cannot open the helper output pipe");
    fcntl(output[0], F_SETFD, FD_CLOEXEC);
    fcntl(output[1], F_SETFD, FD_CLOEXEC);
    std::vector<char *> argv{ const_cast<char *>(executable.c_str()) };
    for (const auto &arg : arguments)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    auto pid = fork();
    if (pid == 0) {
        close(output[0]);
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        close(output[1]);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    close(output[1]);
    if (pid < 0) {
        close(output[0]);
        throw std::runtime_error("Cannot start helper");
    }
    std::string result;
    char buffer[2048];
    ssize_t length;
    do {
        length = read(output[0], buffer, sizeof(buffer));
        if (length > 0) collect(result, buffer, length);
    } while (length > 0 || (length < 0 && errno == EINTR));
    int read_error = length < 0 ? errno : 0;
    close(output[0]);
    int code = 0;
    pid_t waited;
    do { waited = waitpid(pid, &code, 0); }
    while (waited < 0 && errno == EINTR);
    if (read_error || waited < 0)
        throw std::runtime_error("Cannot collect helper output or exit status");
    if (!WIFEXITED(code) || WEXITSTATUS(code) != 0)
        throw std::runtime_error(
                result.empty() ? "Helper failed to start" : result);
    return result;
#endif
}

std::string bundled_helper()
{
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, path.data(), path.size());
    if (!length || length >= path.size())
        throw failure("Cannot locate bundled Crystal helper");
    path.resize(length);
    return (std::filesystem::path(path).parent_path() / "Bridge" /
            "crystal-bridge.exe").u8string();
#else
    return {};
#endif
}

Temporary::Temporary()
{
#ifdef _WIN32
    GUID id;
    wchar_t name[40];
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, 40))
        throw std::runtime_error("Cannot identify temporary helper workspace");
    directory = std::filesystem::temp_directory_path() /
                (std::wstring(L"crystal-goxel-ui-") + name);
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Cannot create temporary helper workspace");
#else
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "crystal-goxel-ui-XXXXXX").string();
    auto *path = mkdtemp(pattern.data());
    if (!path) throw std::runtime_error("Cannot create helper workspace");
    directory = path;
#endif
}

Temporary::~Temporary()
{
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}
}
