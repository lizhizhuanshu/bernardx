#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using AsyncHandle = int64_t;

struct LuaRefBase {
    const int ref;
    const int type;
    virtual ~LuaRefBase() = default;
protected:
    LuaRefBase(int r, int t) : ref(r), type(t) {}
};

using LuaRef = std::shared_ptr<LuaRefBase>;

using LuaValue = std::variant<std::nullptr_t, bool, int64_t, double, std::string, LuaRef>;

struct ScriptResult {
    int status = 2;  // LUA_ERRRUN
    std::vector<LuaValue> values;
    std::string error;
};
