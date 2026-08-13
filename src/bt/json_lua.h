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

// Convert a Lua value at index `idx` to json. Scalars map directly; tables
// become objects, except a table whose keys are all positive integers 1..N
// (contiguous from 1) becomes a json array (1-based → 0-based). Used to read
// `params` tables passed to bt.init, and by json.encode.
//
// For tables: copies the table to the top of the stack first, so recursive
// calls with lua_pushnil/lua_next don't invalidate the index.
inline nlohmann::json LuaToJson(lua_State* L, int idx) {
    luaL_checkstack(L, 4, nullptr);
    int abs = lua_absindex(L, idx);
    int t = lua_type(L, abs);
    switch (t) {
    case LUA_TNIL:
        return nlohmann::json(nullptr);
    case LUA_TBOOLEAN:
        return nlohmann::json(static_cast<bool>(lua_toboolean(L, abs)));
    case LUA_TNUMBER: {
        if (lua_isinteger(L, abs))
            return nlohmann::json(static_cast<int64_t>(lua_tointeger(L, abs)));
        return nlohmann::json(lua_tonumber(L, abs));
    }
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, abs, &len);
        return nlohmann::json(std::string(s, len));
    }
    case LUA_TTABLE: {
        // Push a copy at top of stack so recursive calls don't invalidate the index
        lua_pushvalue(L, abs);
        int table_idx = lua_gettop(L);

        // Check if array (all keys are positive integers starting from 1)
        bool is_array = true;
        lua_Integer max_index = 0;
        lua_pushnil(L);
        while (lua_next(L, table_idx) != 0) {
            lua_pop(L, 1); // pop value, keep key
            if (lua_type(L, -1) != LUA_TNUMBER || !lua_isinteger(L, -1)) {
                is_array = false;
                lua_pop(L, 1); // pop remaining key so stack is clean
                break;
            }
            lua_Integer k = lua_tointeger(L, -1);
            if (k < 1) {
                is_array = false;
                lua_pop(L, 1); // pop remaining key
                break;
            }
            if (k > max_index) max_index = k;
        }

        nlohmann::json result;
        if (is_array && max_index > 0) {
            nlohmann::json arr = nlohmann::json::array();
            for (lua_Integer i = 1; i <= max_index; i++) {
                lua_rawgeti(L, table_idx, i);
                arr.push_back(LuaToJson(L, -1));
                lua_pop(L, 1);
            }
            result = std::move(arr);
        } else {
            // Object
            nlohmann::json obj = nlohmann::json::object();
            lua_pushnil(L);
            while (lua_next(L, table_idx) != 0) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    key = lua_tostring(L, -2);
                } else if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
                    key = std::to_string(lua_tointeger(L, -2));
                }
                obj[key] = LuaToJson(L, -1);
                lua_pop(L, 1);
            }
            result = std::move(obj);
        }

        lua_pop(L, 1); // pop the table copy
        return result;
    }
    default:
        return nlohmann::json(nullptr);
    }
}
