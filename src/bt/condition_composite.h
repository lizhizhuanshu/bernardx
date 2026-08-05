#pragma once

#include <memory>
#include <vector>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"
#include "node_condition.h"

// Logical AND: all sub-conditions must be met. Short-circuits on the first
// Failure (returns Failure) or the first Running (returns Running, leaving
// later subs un-ticked this evaluation). All Success -> Success.
class AndCondition : public NodeCondition {
public:
    AndCondition() : NodeCondition("And") {}
    void AddChild(std::unique_ptr<NodeCondition> child);
    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;

private:
    std::vector<std::unique_ptr<NodeCondition>> children_;
};

// Logical OR: any sub-condition met. Short-circuits on the first Success or
// Running. All Failure -> Failure.
class OrCondition : public NodeCondition {
public:
    OrCondition() : NodeCondition("Or") {}
    void AddChild(std::unique_ptr<NodeCondition> child);
    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;

private:
    std::vector<std::unique_ptr<NodeCondition>> children_;
};

// Logical NOT: inverts Success/Failure of its single sub; Running passes
// through unchanged.
class NotCondition : public NodeCondition {
public:
    explicit NotCondition(std::unique_ptr<NodeCondition> child)
        : NodeCondition("Not"), child_(std::move(child)) {}
    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;

private:
    std::unique_ptr<NodeCondition> child_;
};
