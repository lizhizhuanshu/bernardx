#include "file_system_resource_provider.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

FileSystemResourceProvider::FileSystemResourceProvider(std::string resource_dir)
    : base_dir_(std::move(resource_dir)) {
    fs::create_directories(base_dir_);
}

async_simple::coro::Lazy<std::vector<ResourceEntry>> FileSystemResourceProvider::List(const std::string& relative_path) {
    std::vector<ResourceEntry> entries;
    std::error_code ec;
    auto dir = base_dir_;
    if (!relative_path.empty()) {
        dir += "/" + relative_path;
    }

    if (!fs::is_directory(dir, ec)) {
        co_return entries;
    }

    for (auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        ResourceEntry e;
        e.name = entry.path().filename().string();
        e.path = relative_path.empty() ? e.name : (relative_path + "/" + e.name);
        e.is_directory = entry.is_directory(ec);
        if (ec) {
            e.is_directory = false;
            ec.clear();
        }
        entries.push_back(std::move(e));
    }
    co_return entries;
}

async_simple::coro::Lazy<std::optional<std::string>> FileSystemResourceProvider::Load(const std::string& relative_path) {
    auto full = base_dir_ + "/" + relative_path;
    std::ifstream ifs(full, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        co_return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    co_return content;
}
