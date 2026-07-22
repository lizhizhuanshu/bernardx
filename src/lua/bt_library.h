#pragma once

#include <memory>

#include "behavior_tree_engine.h"
#include "lua_library.h"
#include "lua_runtime.h"

class Blackboard;

class BehaviorTreeLibrary : public LuaLibrary {
public:
    explicit BehaviorTreeLibrary(std::shared_ptr<Blackboard> bb);
    ~BehaviorTreeLibrary() override;

    BehaviorTreeLibrary(const BehaviorTreeLibrary&) = delete;
    BehaviorTreeLibrary& operator=(const BehaviorTreeLibrary&) = delete;
    BehaviorTreeLibrary(BehaviorTreeLibrary&&) = delete;
    BehaviorTreeLibrary& operator=(BehaviorTreeLibrary&&) = delete;

    std::string name() const override { return "bt"; }
    void Open(lua_State* L) override;
    void Close(lua_State* L) override;

    BehaviorTreeEngine::Ptr engine() const { return engine_; }

private:
    BehaviorTreeEngine::Ptr engine_;
};
