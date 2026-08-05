#include "condition_composite.h"

void AndCondition::AddChild(std::unique_ptr<NodeCondition> child) {
    children_.push_back(std::move(child));
}

async_simple::coro::Lazy<bool> AndCondition::Init(lua_State* L, LuaRuntime* ctx) {
    for (auto& c : children_) {
        if (!co_await c->Init(L, ctx)) {
            set_last_error(c->last_error());
            co_return false;
        }
    }
    co_return true;
}

NodeStatus AndCondition::Tick(Blackboard& bb, BtEventQueue& events) {
    for (auto& c : children_) {
        auto s = c->Tick(bb, events);
        if (s != NodeStatus::kSuccess) return s;  // Failure or Running short-circuits
    }
    return NodeStatus::kSuccess;
}

void AndCondition::Reset() {
    for (auto& c : children_) c->Reset();
}

void OrCondition::AddChild(std::unique_ptr<NodeCondition> child) {
    children_.push_back(std::move(child));
}

async_simple::coro::Lazy<bool> OrCondition::Init(lua_State* L, LuaRuntime* ctx) {
    for (auto& c : children_) {
        if (!co_await c->Init(L, ctx)) {
            set_last_error(c->last_error());
            co_return false;
        }
    }
    co_return true;
}

NodeStatus OrCondition::Tick(Blackboard& bb, BtEventQueue& events) {
    for (auto& c : children_) {
        auto s = c->Tick(bb, events);
        if (s != NodeStatus::kFailure) return s;  // Success or Running short-circuits
    }
    return NodeStatus::kFailure;
}

void OrCondition::Reset() {
    for (auto& c : children_) c->Reset();
}

async_simple::coro::Lazy<bool> NotCondition::Init(lua_State* L, LuaRuntime* ctx) {
    co_return child_ ? co_await child_->Init(L, ctx) : true;
}

NodeStatus NotCondition::Tick(Blackboard& bb, BtEventQueue& events) {
    auto s = child_->Tick(bb, events);
    if (s == NodeStatus::kSuccess) return NodeStatus::kFailure;
    if (s == NodeStatus::kFailure) return NodeStatus::kSuccess;
    return NodeStatus::kRunning;  // pass through
}

void NotCondition::Reset() {
    if (child_) child_->Reset();
}
