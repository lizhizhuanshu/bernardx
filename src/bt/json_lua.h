#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <nlohmann/json.hpp>

#include "lua_runtime.h"
#include "lua_types.h"

// Bridge nlohmann::json values to Lua. Used at Node::Init time (when a live
// lua_State is available) to turn Script `params` — which may be
// objects or arrays — into real Lua tables (LuaRef). Scalars map the same way
// the old ParseLuaValue did.

// Push any json value onto `L` without creating a registry ref. Scalars map
// directly; objects/arrays become nested tables (objects use string field
// keys, arrays use 1-based integer keys, matching Lua convention); null -> nil.
inline void PushJson(lua_State* L, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null:
            lua_pushnil(L);
            return;
        case nlohmann::json::value_t::boolean:
            lua_pushboolean(L, j.get<bool>() ? 1 : 0);
            return;
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            lua_pushinteger(L, static_cast<lua_Integer>(j.get<int64_t>()));
            return;
        case nlohmann::json::value_t::number_float:
            lua_pushnumber(L, j.get<double>());
            return;
        case nlohmann::json::value_t::string: {
            const std::string& s = j.get_ref<const std::string&>();
            lua_pushlstring(L, s.c_str(), s.size());
            return;
        }
        case nlohmann::json::value_t::object:
            lua_newtable(L);
            for (auto it = j.begin(); it != j.end(); ++it) {
                PushJson(L, it.value());
                lua_setfield(L, -2, it.key().c_str());
            }
            return;
        case nlohmann::json::value_t::array: {
            lua_newtable(L);
            lua_Integer idx = 1;
            for (auto it = j.begin(); it != j.end(); ++it, ++idx) {
                PushJson(L, it.value());
                lua_rawseti(L, -2, idx);
            }
            return;
        }
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            lua_pushnil(L);  // not representable as a Lua value
            return;
    }
    lua_pushnil(L);
}

// Convert a json value to a LuaValue. Scalars map directly; objects/arrays are
// built on `L` via PushJson and referenced with luaL_ref (only the top-level
// table gets a registry ref — nested tables are built in place), returning a
// LuaRef the caller can store and later push via PushValues. `rt` must outlive
// the returned LuaRef (the LuaRef releases the registry slot on destruction).
inline LuaValue JsonToLuaValue(lua_State* L, LuaRuntime* rt, const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::null:
            return LuaValue(nullptr);
        case nlohmann::json::value_t::boolean:
            return LuaValue(j.get<bool>());
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            return LuaValue(static_cast<int64_t>(j.get<int64_t>()));
        case nlohmann::json::value_t::number_float:
            return LuaValue(j.get<double>());
        case nlohmann::json::value_t::string:
            return LuaValue(j.get_ref<const std::string&>());
        case nlohmann::json::value_t::object:
        case nlohmann::json::value_t::array: {
            PushJson(L, j);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the table
            if (ref == LUA_NOREF || ref == LUA_REFNIL) return LuaValue(nullptr);
            return LuaValue(rt->CreateRef(ref, LUA_TTABLE));
        }
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            return LuaValue(nullptr);
    }
    return LuaValue(nullptr);
}
