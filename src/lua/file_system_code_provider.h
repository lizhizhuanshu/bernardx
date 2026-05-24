#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "code_provider.h"

// FileSystemCodeProvider: loads Lua modules from configurable search paths.
// Default constructor: base_dir → searches {base}/src/ and {base}/libs/
// Explicit constructor: uses provided search paths directly.
class FileSystemCodeProvider : public CodeProvider {
public:
    // Search {base}/src/ then {base}/libs/
    explicit FileSystemCodeProvider(const std::string& base_dir) {
        auto abs = std::filesystem::absolute(base_dir).string();
        search_paths_ = {
            abs + "/src",
            abs + "/libs",
        };
    }

    explicit FileSystemCodeProvider(std::vector<std::string> search_paths) {
        search_paths_.reserve(search_paths.size());
        for (auto& p : search_paths) {
            search_paths_.push_back(std::filesystem::absolute(p).string());
        }
    }

    static std::string ModuleToPath(const std::string& module_name) {
        std::string path = module_name;
        std::replace(path.begin(), path.end(), '.', '/');
        return path;
    }

    async_simple::coro::Lazy<std::optional<std::string>> LoadModule(const std::string& module_name) override {
        auto path = ModuleToPath(module_name);
        if (auto result = TryLoad(path + ".lua")) {
            co_return result;
        }
        co_return TryLoad(path + "/init.lua");
    }

    async_simple::coro::Lazy<std::optional<std::string>> LoadFile(const std::string& path) override {
        co_return TryLoad(path);
    }

    const std::vector<std::string>& search_paths() const { return search_paths_; }

private:
    std::optional<std::string> TryLoad(const std::string& filename) const {
        for (const auto& dir : search_paths_) {
            auto full = dir + "/" + filename;
            std::ifstream ifs(full, std::ios::in | std::ios::binary);
            if (ifs.is_open()) {
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
                return content;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> search_paths_;
};
