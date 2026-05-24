#pragma once

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_runtime.h"

[[nodiscard]] inline LuaValue LuaValueFromStack(lua_State* L, int idx) {
    int t = lua_type(L, idx);
    if (t == LUA_TNIL) {
        return LuaValue(nullptr);
    } else if (t == LUA_TBOOLEAN) {
        return LuaValue(static_cast<bool>(lua_toboolean(L, idx)));
    } else if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, idx)) {
            return LuaValue(static_cast<int64_t>(lua_tointeger(L, idx)));
        }
        return LuaValue(lua_tonumber(L, idx));
    } else if (t == LUA_TSTRING) {
        size_t len;
        const char* s = lua_tolstring(L, idx, &len);
        return LuaValue(std::string(s, len));
    } else {
        int abs_idx = lua_absindex(L, idx);
        lua_pushvalue(L, abs_idx);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        auto rt = LuaRuntime::FromLuaState(L);
        if (rt) {
            return LuaValue(rt->CreateRef(ref, t));
        }
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return LuaValue(nullptr);
    }
}

[[nodiscard]] inline LuaValue PopLuaValue(lua_State* L, int idx) {
    return LuaValueFromStack(L, idx);
}

inline void PushLuaValue(lua_State* L, const LuaValue& v) {
    LuaRuntime::PushValues(L, {v});
}
