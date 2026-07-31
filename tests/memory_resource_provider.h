#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "resource_provider.h"

// Test-only ResourceProvider backed by an in-memory path -> content map.
// Register JSON content with Put(), reference it from Lua via "@<path>".
class MemoryResourceProvider : public ResourceProvider {
public:
    void Put(const std::string& path, std::string content) {
        entries_[path] = std::move(content);
    }

    async_simple::coro::Lazy<std::vector<ResourceEntry>> List(const std::string& /*relative_path*/) override {
        // Directory listing is not needed by the JSON tree loader in tests.
        co_return std::vector<ResourceEntry>{};
    }

    async_simple::coro::Lazy<std::optional<std::string>> Load(const std::string& relative_path) override {
        auto it = entries_.find(relative_path);
        if (it != entries_.end()) co_return it->second;
        co_return std::nullopt;
    }

private:
    std::unordered_map<std::string, std::string> entries_;
};
