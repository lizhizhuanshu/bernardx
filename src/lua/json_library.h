#pragma once

#include "lua_library.h"

class JsonLibrary : public LuaLibrary {
public:
    std::string name() const override { return "json"; }
    void Open(lua_State* L) override;
};
