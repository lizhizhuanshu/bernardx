#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

extern "C" {
#include "lua.h"
}

using AsyncHandle = int64_t;

struct lua_State;

struct LuaRefBase {
    const int ref;
    const int type;
    virtual ~LuaRefBase() = default;
    // The state owning this registry ref (nullptr for refs created outside a
    // runtime). Lets holders index a table ref without carrying a lua_State.
    virtual lua_State* state() const { return nullptr; }
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
    // True when the error was raised from inside a C function (level-0 frame
    // is "=[C]"). Such messages carry no "source:line:" prefix (luaL_where is
    // empty in C frames), so formatters synthesize a location from the
    // nearest Lua frame (source/line above already point at it).
    bool raised_in_c = false;
};

struct ScriptResult {
    int status = 2;  // LUA_ERRRUN
    std::vector<LuaValue> values;
    std::string error;
    ScriptErrorDetail error_detail;
};
