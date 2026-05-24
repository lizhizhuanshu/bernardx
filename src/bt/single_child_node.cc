#include "single_child_node.h"

SingleChildNode::SingleChildNode(uint32_t id, std::string type, std::string name,
                                 std::unique_ptr<Node> child)
    : Node(id, std::move(type), std::move(name)), child_(std::move(child)) {
    if (child_) child_->set_parent(this);
}

void SingleChildNode::Reset() {
    if (child_) child_->Reset();
    Node::Reset();
}

void SingleChildNode::OnAborted() {
    if (child_) child_->OnAborted();
    Node::OnAborted();
}

async_simple::coro::Lazy<bool> SingleChildNode::Init(lua_State* L, LuaRuntime* ctx,
                                                      const std::string& base_path) {
    if (child_ && !co_await child_->Init(L, ctx, base_path)) {
        set_last_error(child_->last_error());
        co_return false;
    }
    co_return true;
}
