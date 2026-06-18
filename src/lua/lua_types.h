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

struct ScriptErrorDetail {
    std::string source;      // Lua chunk name (e.g. "@scripts/task.lua" or "=script")
    int line = -1;           // line number where error occurred
    std::string message;     // original Lua error message
    std::string stack_trace; // "file:line -> file:line -> ..."
};

struct ScriptResult {
    int status = 2;  // LUA_ERRRUN
    std::vector<LuaValue> values;
    std::string error;
    ScriptErrorDetail error_detail;
};
