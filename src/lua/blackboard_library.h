#pragma once

#include <memory>

#include "lua_library.h"

class Blackboard;

class BlackboardLibrary : public LuaLibrary {
public:
    explicit BlackboardLibrary(std::shared_ptr<Blackboard> bb);

    std::string name() const override { return "blackboard"; }
    void Open(lua_State* L) override;

    std::shared_ptr<Blackboard> blackboard() const { return blackboard_; }

private:
    std::shared_ptr<Blackboard> blackboard_;
};
