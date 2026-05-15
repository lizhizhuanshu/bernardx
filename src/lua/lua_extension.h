#pragma once

extern "C" {
#include "lua.h"
}

class LuaExtension {
public:
    virtual ~LuaExtension() = default;
    virtual void OnInit(lua_State* L) = 0;
    virtual void OnShutdown(lua_State* L) = 0;
};