#pragma once

#include <async_simple/coro/Lazy.h>
#include <optional>
#include <string>

class CodeProvider {
public:
    virtual ~CodeProvider() = default;
    virtual async_simple::coro::Lazy<std::optional<std::string>> LoadModule(const std::string& module_name) = 0;
    virtual async_simple::coro::Lazy<std::optional<std::string>> LoadFile(const std::string& path) = 0;
};
