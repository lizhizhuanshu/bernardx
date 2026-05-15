#pragma once

extern "C" {
#include "lua.h"
}

#include <string>

class LuaLibrary {
public:
    virtual ~LuaLibrary() = default;
    virtual std::string name() const = 0;
    virtual void Open(lua_State* L) = 0;
    virtual void Close(lua_State* L) {}
};
