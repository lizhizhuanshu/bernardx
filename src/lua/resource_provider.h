#pragma once

#include <async_simple/coro/Lazy.h>
#include <optional>
#include <string>
#include <vector>

struct ResourceEntry {
    std::string name;
    std::string path;
    bool is_directory = false;
};

class ResourceProvider {
public:
    virtual ~ResourceProvider() = default;
    virtual async_simple::coro::Lazy<std::vector<ResourceEntry>> List(const std::string& relative_path) = 0;
    virtual async_simple::coro::Lazy<std::optional<std::string>> Load(const std::string& relative_path) = 0;
};
