#pragma once

#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"

struct ScriptRefs {
    int table_ref = LUA_NOREF;
    int enter_ref = LUA_NOREF;
    int tick_ref = LUA_NOREF;
    int exit_ref = LUA_NOREF;
    int abort_ref = LUA_NOREF;
};

class LuaScriptHost {
public:
    ~LuaScriptHost();

    async_simple::coro::Lazy<bool> LoadScript(
        lua_State* L, LuaRuntime* ctx,
        const std::string& base_path,
        const std::string& script_path,
        bool require_abort);

    bool is_loaded() const { return refs_.tick_ref != LUA_NOREF; }

    const std::string& last_error() const { return last_error_; }

    lua_State* main_L_ = nullptr;
    LuaRuntime* lua_context_ = nullptr;
    ScriptRefs refs_;
    std::string last_error_;
};
