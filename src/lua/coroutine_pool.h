#pragma once

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <functional>
#include <unordered_map>
#include <vector>

class CoroutinePool {
public:
    using OnCoroutineCreated = std::function<void(lua_State* co)>;

    CoroutinePool(lua_State* main_L, OnCoroutineCreated on_created);

    lua_State* Acquire();
    void Release(lua_State* co);

    void Shutdown(lua_State* main_L);

private:
    std::unordered_map<lua_State*, int> active_co_refs_;
    std::vector<std::pair<lua_State*, int>> co_pool_;
    lua_State* main_L_;
    OnCoroutineCreated on_created_;
};
