#include "blackboard_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

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
        {"get", bb_get},
        {"has", bb_has},
        {"remove", bb_remove},
        {"clear", bb_clear},
        {"to_table", bb_to_table},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 1);
}
