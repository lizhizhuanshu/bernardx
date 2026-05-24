#include "node.h"

#include "lua_runtime.h"

Node::Node(uint32_t id, std::string type, std::string name)
    : id_(id), type_(std::move(type)), name_(std::move(name)) {}

void Node::Reset() {
    last_error_.clear();
}

void Node::OnAborted() {
}

async_simple::coro::Lazy<bool> Node::Init(lua_State* /*L*/, LuaRuntime* /*ctx*/,
                                            const std::string& /*base_path*/) {
    co_return true;
}

void Node::AddDecorator(std::unique_ptr<Decorator> dec) {
    decorators_.push_back(std::move(dec));
}
