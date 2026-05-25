#pragma once

#include "lua_library.h"

class FileSystemLibrary : public LuaLibrary {
public:
    std::string name() const override { return "lfs"; }
    void Open(lua_State* L) override;
};
