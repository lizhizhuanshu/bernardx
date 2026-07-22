#include "lua_script_host.h"

#include <spdlog/spdlog.h>

#include "bt_utils.h"

async_simple::coro::Lazy<bool> LuaScriptHost::LoadScript(
    lua_State* L, LuaRuntime* ctx,
    const std::string& script_path,
    bool require_abort) {
    main_L_ = L;
    lua_context_ = ctx;

    if (!ctx->shared_code_provider()) {
        last_error_ = "no code provider configured, cannot load '" + script_path + "'";
        spdlog::error("LuaScriptHost::LoadScript: {}", last_error_);
        co_return false;
    }

    auto source = co_await ctx->shared_code_provider()->LoadFile(script_path);
    if (!source.has_value()) {
        last_error_ = "failed to load script '" + script_path + "'";
        spdlog::error("LuaScriptHost::LoadScript: {}", last_error_);
        co_return false;
    }

    auto result = co_await ctx->DoBufferAsync(script_path, std::move(*source));

    if (result.status != LUA_OK) {
        last_error_ = "failed to execute '" + script_path + "': " +
                      (result.error.empty() ? "unknown error" : result.error);
        spdlog::error("LuaScriptHost::LoadScript: {}", last_error_);
        co_return false;
    }

    if (result.values.empty()) {
        last_error_ = "'" + script_path + "' did not return a value";
        spdlog::error("LuaScriptHost::LoadScript: {}", last_error_);
        co_return false;
    }

    auto* table_ref = std::get_if<LuaRef>(&result.values[0]);
    if (!table_ref) {
        last_error_ = "'" + script_path + "' did not return a table";
        spdlog::error("LuaScriptHost::LoadScript: {}", last_error_);
        co_return false;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, (*table_ref)->ref);
    if (!lua_istable(L, -1)) {
        last_error_ = "'" + script_path + "' did not return a table";
        spdlog::error("LuaScriptHost::LoadScript: {}", last_error_);
        lua_pop(L, 1);
        co_return false;
    }

    lua_pushvalue(L, -1);
    refs_.table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    int table_idx = lua_absindex(L, -1);

    auto get_ref = [&](const char* name) -> int {
        lua_getfield(L, table_idx, name);
        int ref = LUA_NOREF;
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -1);
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        lua_pop(L, 1);
        return ref;
    };

    refs_.enter_ref = get_ref("Enter");
    refs_.tick_ref = get_ref("Tick");
    refs_.exit_ref = get_ref("Exit");
    if (require_abort) {
        refs_.abort_ref = get_ref("Abort");
    }

    lua_pop(L, 1);

    if (refs_.tick_ref == LUA_NOREF) {
        last_error_ = "'" + script_path + "' missing required 'Tick' function";
        spdlog::error("LuaScriptHost::LoadScript: {}", last_error_);
        co_return false;
    }

    spdlog::info("LuaScriptHost::LoadScript: loaded '{}' (Enter={}, Tick={}, Exit={}, Abort={})",
                 script_path,
                 refs_.enter_ref != LUA_NOREF, refs_.tick_ref != LUA_NOREF,
                 refs_.exit_ref != LUA_NOREF, refs_.abort_ref != LUA_NOREF);
    co_return true;
}

LuaScriptHost::~LuaScriptHost() {
    if (!main_L_) return;
    ReleaseLuaRef(main_L_, refs_.table_ref);
    ReleaseLuaRef(main_L_, refs_.enter_ref);
    ReleaseLuaRef(main_L_, refs_.tick_ref);
    ReleaseLuaRef(main_L_, refs_.exit_ref);
    ReleaseLuaRef(main_L_, refs_.abort_ref);
}
