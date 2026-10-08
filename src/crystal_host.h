// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace crystal_host {
std::string run(const std::string &executable,
                const std::vector<std::string> &arguments);
std::string bundled_helper();

struct Temporary {
    std::filesystem::path directory;
    Temporary();
    ~Temporary();
    Temporary(const Temporary &) = delete;
    Temporary &operator=(const Temporary &) = delete;
};
}
