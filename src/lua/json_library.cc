#include "json_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <nlohmann/json.hpp>

#include <string>

#include "json_lua.h"

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
// Implemented in json_lua.h (shared with bt.init params reading).
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
    auto j = LuaToJson(L, 1);
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
