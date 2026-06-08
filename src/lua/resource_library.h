#pragma once

#include "lua_library.h"
#include "resource_provider.h"

#include <memory>

class ResourceLibrary : public LuaLibrary {
public:
    explicit ResourceLibrary(std::shared_ptr<ResourceProvider> provider);
    std::string name() const override { return "resource"; }
    void Open(lua_State* L) override;
private:
    std::shared_ptr<ResourceProvider> provider_;
};
