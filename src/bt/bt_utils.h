#pragma once

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <spdlog/spdlog.h>

#include "lua_runtime.h"
#include "time_utils.h"

inline void ReleaseLuaRef(lua_State* L, int& ref) {
    if (ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    }
}

inline void LuaCallMethod(lua_State* L, int fn_ref, int self_ref, int extra_args) {
    int base = lua_gettop(L) - extra_args + 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    lua_insert(L, base);
    lua_rawgeti(L, LUA_REGISTRYINDEX, self_ref);
    lua_insert(L, base + 1);
    if (lua_pcall(L, 1 + extra_args, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("LuaCallMethod: error: {}", err ? err : "unknown");
        lua_pop(L, 1);
    }
}

inline void PushArgsTable(lua_State* L,
                          const std::unordered_map<std::string, LuaValue>& args) {
    lua_newtable(L);
    for (const auto& [key, value] : args) {
        lua_pushstring(L, key.c_str());
        LuaRuntime::PushValues(L, {value});
        lua_settable(L, -3);
    }
}

class ShuffledIndexTracker {
public:
    void EnsureShuffled(size_t count) {
        if (shuffled_) return;
        order_.resize(count);
        for (size_t i = 0; i < order_.size(); ++i) {
            order_[i] = i;
        }
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(order_.begin(), order_.end(), g);
        shuffled_ = true;
    }

    void Reset() {
        shuffled_ = false;
        order_.clear();
    }

    const std::vector<size_t>& order() const { return order_; }
    bool shuffled() const { return shuffled_; }

private:
    std::vector<size_t> order_;
    bool shuffled_ = false;
};
