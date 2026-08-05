#include "node.h"

#include "lua_runtime.h"

Node::Node(uint32_t id, std::string type, std::string name)
    : id_(id), type_(std::move(type)), name_(std::move(name)) {}

void Node::Reset() {
    last_error_.clear();
    is_running_ = false;
    if (condition_) condition_->Reset();
}

void Node::OnAborted() {
    if (condition_) condition_->Reset();
}

async_simple::coro::Lazy<bool> Node::Init(lua_State* /*L*/, LuaRuntime* /*ctx*/) {
    co_return true;
}
