#include "blackboard_library.h"

#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "lua_value_utils.h"

namespace {

Blackboard* GetBB(lua_State* L) {
    return static_cast<Blackboard*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int bb_set(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    auto value = PopLuaValue(L, 2);
    bb->Set(key, std::move(value));
    return 0;
}

// bb.set_provider(key, fn): install `fn` as the computed-value source for
// `key`. Every later bb.get(key) - and every engine-side read ($key param
// resolution at Enter, Blackboard condition comparisons) - invokes fn fresh
// and uses its return value. On a DOTTED read the remaining path segments
// are passed to fn as string arguments: bb.get("cfg.net.ip") calls
// fn("net","ip"); a flat bb.get("cfg") calls fn() with no args. fn must
// be synchronous (no yield/sleep/await); a nil return is a present nil; a
// runtime error logs and yields nil. The provider replaces any static value
// (and bb.set replaces a provider); bb.remove / bb.clear drop it. Replacing
// or removing releases the function ref.
int bb_set_provider(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto rt = LuaRuntime::FromLuaState(L);
    if (!rt) {
        return luaL_error(L, "blackboard provider requires a LuaRuntime");
    }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    LuaRef fn = rt->CreateRef(ref, LUA_TFUNCTION);
    lua_State* main_L = rt->main_state();
    int fn_ref = fn->ref;  // stable while `fn` is alive
    bb->SetProvider(key, [fn = std::move(fn), main_L, fn_ref](
                             const std::vector<std::string>& path) -> LuaValue {
        lua_rawgeti(main_L, LUA_REGISTRYINDEX, fn_ref);
        for (const auto& seg : path) {
            lua_pushlstring(main_L, seg.data(), seg.size());
        }
        if (lua_pcall(main_L, static_cast<int>(path.size()), 1, 0) != LUA_OK) {
            const char* err = lua_tostring(main_L, -1);
            spdlog::error("blackboard provider error: {}", err ? err : "unknown error");
            lua_pop(main_L, 1);
            return LuaValue(nullptr);
        }
        LuaValue v = LuaValueFromStack(main_L, -1);
        lua_pop(main_L, 1);
        return v;
    });
    return 0;
}

int bb_get(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    auto value = bb->Get(key);
    if (value.has_value()) {
        PushLuaValue(L, *value);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int bb_has(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    lua_pushboolean(L, bb->Has(key));
    return 1;
}

int bb_remove(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    bb->Remove(key);
    return 0;
}

int bb_clear(lua_State* L) {
    GetBB(L)->Clear();
    return 0;
}

int bb_to_table(lua_State* L) {
    GetBB(L)->PushAsTable(L);
    return 1;
}

}  // namespace

BlackboardLibrary::BlackboardLibrary(std::shared_ptr<Blackboard> bb)
    : blackboard_(std::move(bb)) {}

void BlackboardLibrary::Open(lua_State* L) {
    lua_newtable(L);

    lua_pushlightuserdata(L, blackboard_.get());

    luaL_Reg funcs[] = {
        {"set", bb_set},
        {"set_provider", bb_set_provider},
        {"get", bb_get},
        {"has", bb_has},
        {"remove", bb_remove},
        {"clear", bb_clear},
        {"to_table", bb_to_table},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 1);
}
