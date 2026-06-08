#include "resource_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_types.h"
#include "lua_runtime.h"

#include <string>
#include <vector>

static async_simple::coro::Lazy<void> ResumeResourceList(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    ResourceProvider* provider,
    std::string path) {
    auto entries = co_await provider->List(path);

    auto* L = rt->main_state();
    lua_newtable(L);
    for (size_t i = 0; i < entries.size(); ++i) {
        lua_newtable(L);
        lua_pushlstring(L, entries[i].name.c_str(), entries[i].name.size());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, entries[i].is_directory);
        lua_setfield(L, -2, "is_dir");
        if (!entries[i].path.empty()) {
            lua_pushlstring(L, entries[i].path.c_str(), entries[i].path.size());
            lua_setfield(L, -2, "path");
        }
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    rt->PushResume(handle, {LuaValue{rt->CreateRef(ref, LUA_TTABLE)}});
}

static async_simple::coro::Lazy<void> ResumeResourceLoad(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    ResourceProvider* provider,
    std::string path) {
    auto result = co_await provider->Load(path);
    if (result.has_value()) {
        rt->PushResume(handle, {LuaValue{std::move(*result)}});
    } else {
        rt->PushResume(handle, {LuaValue{nullptr}});
    }
}

static int resource_ls(lua_State* L) {
    auto* provider = static_cast<ResourceProvider*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* path = luaL_optstring(L, 1, "");

    auto rt = LuaRuntime::FromLuaState(L);
    if (!rt) {
        return luaL_error(L, "resource.ls: no LuaRuntime");
    }
    if (!rt->executor()) {
        return luaL_error(L, "resource.ls: executor required");
    }

    auto handle = rt->PreYield(L);
    ResumeResourceList(rt, handle, provider, std::string(path))
        .via(rt->executor())
        .detach();

    return lua_yield(L, 0);
}

static int resource_load(lua_State* L) {
    auto* provider = static_cast<ResourceProvider*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* path = luaL_checkstring(L, 1);

    auto rt = LuaRuntime::FromLuaState(L);
    if (!rt) {
        return luaL_error(L, "resource.load: no LuaRuntime");
    }
    if (!rt->executor()) {
        return luaL_error(L, "resource.load: executor required");
    }

    auto handle = rt->PreYield(L);
    ResumeResourceLoad(rt, handle, provider, std::string(path))
        .via(rt->executor())
        .detach();

    return lua_yield(L, 0);
}

ResourceLibrary::ResourceLibrary(std::shared_ptr<ResourceProvider> provider)
    : provider_(std::move(provider)) {}

void ResourceLibrary::Open(lua_State* L) {
    lua_newtable(L);
    lua_pushlightuserdata(L, provider_.get());
    luaL_Reg funcs[] = {
        {"ls",   resource_ls},
        {"load", resource_load},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, funcs, 1);
}
