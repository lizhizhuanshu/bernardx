#include "json_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

// --- Push a JSON value onto the Lua stack ---
static void push_json_value(lua_State* L, const json& val) {
    luaL_checkstack(L, 4, nullptr);
    switch (val.type()) {
    case json::value_t::null:
        lua_pushnil(L);
        break;
    case json::value_t::boolean:
        lua_pushboolean(L, val.get<bool>());
        break;
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
        lua_pushinteger(L, static_cast<lua_Integer>(val.get<int64_t>()));
        break;
    case json::value_t::number_float:
        lua_pushnumber(L, val.get<double>());
        break;
    case json::value_t::string:
        lua_pushstring(L, val.get_ref<const std::string&>().c_str());
        break;
    case json::value_t::array: {
        lua_createtable(L, static_cast<int>(val.size()), 0);
        int i = 1;
        for (const auto& elem : val) {
            push_json_value(L, elem);
            lua_rawseti(L, -2, i++);
        }
        break;
    }
    case json::value_t::object: {
        lua_createtable(L, 0, static_cast<int>(val.size()));
        for (auto it = val.begin(); it != val.end(); ++it) {
            lua_pushstring(L, it.key().c_str());
            push_json_value(L, it.value());
            lua_rawset(L, -3);
        }
        break;
    }
    default:
        lua_pushnil(L);
        break;
    }
}

// --- Convert a Lua value at given index to JSON ---
// For tables: copies the table to the top of the stack first, so recursive
// calls with lua_pushnil/lua_next don't invalidate the index.
static json lua_to_json(lua_State* L, int idx) {
    luaL_checkstack(L, 4, nullptr);
    int abs = lua_absindex(L, idx);
    int t = lua_type(L, abs);
    switch (t) {
    case LUA_TNIL:
        return json(nullptr);
    case LUA_TBOOLEAN:
        return json(static_cast<bool>(lua_toboolean(L, abs)));
    case LUA_TNUMBER: {
        if (lua_isinteger(L, abs))
            return json(static_cast<int64_t>(lua_tointeger(L, abs)));
        return json(lua_tonumber(L, abs));
    }
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, abs, &len);
        return json(std::string(s, len));
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

        json result;
        if (is_array && max_index > 0) {
            json arr = json::array();
            for (lua_Integer i = 1; i <= max_index; i++) {
                lua_rawgeti(L, table_idx, i);
                arr.push_back(lua_to_json(L, -1));
                lua_pop(L, 1);
            }
            result = std::move(arr);
        } else {
            // Object
            json obj = json::object();
            lua_pushnil(L);
            while (lua_next(L, table_idx) != 0) {
                std::string key;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    key = lua_tostring(L, -2);
                } else if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
                    key = std::to_string(lua_tointeger(L, -2));
                }
                obj[key] = lua_to_json(L, -1);
                lua_pop(L, 1);
            }
            result = std::move(obj);
        }

        lua_pop(L, 1); // pop the table copy
        return result;
    }
    default:
        return json(nullptr);
    }
}

// json.decode(str) -> table/value
static int json_decode(lua_State* L) {
    size_t len;
    const char* str = luaL_checklstring(L, 1, &len);
    try {
        auto j = json::parse(std::string_view(str, len));
        push_json_value(L, j);
    } catch (const json::parse_error& e) {
        luaL_error(L, "json parse error: %s", e.what());
        return 0; // unreachable
    }
    return 1;
}

// json.encode(value [, indent]) -> string
static int json_encode(lua_State* L) {
    auto j = lua_to_json(L, 1);
    int indent = lua_isinteger(L, 2) ? static_cast<int>(lua_tointeger(L, 2)) : -1;
    std::string output = (indent >= 0) ? j.dump(indent) : j.dump();
    lua_pushlstring(L, output.data(), output.size());
    return 1;
}

// --- JsonLibrary ---

void JsonLibrary::Open(lua_State* L) {
    lua_newtable(L);
    luaL_Reg funcs[] = {
        {"decode", json_decode},
        {"encode", json_encode},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, funcs, 0);
}
