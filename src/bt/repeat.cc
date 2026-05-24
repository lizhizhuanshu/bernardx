#include "repeat.h"

Repeat::Repeat(uint32_t id, std::string name, int count,
               std::unique_ptr<Node> child)
    : Node(id, "Repeat", std::move(name)),
      child_(std::move(child)),
      max_count_(count) {
    if (child_) child_->set_parent(this);
}

NodeStatus Repeat::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!child_) {
        set_last_error("no child node");
        return NodeStatus::kFailure;
    }

    if (max_count_ == kInfinite) {
        auto status = child_->Tick(bb, events);
        if (status == NodeStatus::kFailure) {
            set_last_error(child_->last_error());
            return NodeStatus::kFailure;
        }
        if (status == NodeStatus::kRunning) {
            return NodeStatus::kRunning;
        }
        // Success: reset child and keep going
        child_->Reset();
        return NodeStatus::kRunning;
    }

    // Finite repeat: count executions
    if (current_count_ >= max_count_) {
        return NodeStatus::kSuccess;
    }

    auto status = child_->Tick(bb, events);
    if (status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
    }
    if (status == NodeStatus::kFailure) {
        set_last_error(child_->last_error());
        return NodeStatus::kFailure;
    }

    // Child succeeded: increment count, reset child for next iteration
    ++current_count_;
    child_->Reset();

    if (current_count_ >= max_count_) {
        return NodeStatus::kSuccess;
    }
    return NodeStatus::kRunning;
}

void Repeat::Reset() {
    current_count_ = 0;
    if (child_) child_->Reset();
    Node::Reset();
}

void Repeat::OnAborted() {
    if (child_) child_->OnAborted();
    Node::OnAborted();
}

async_simple::coro::Lazy<bool> Repeat::Init(lua_State* L, LuaRuntime* ctx,
                                              const std::string& base_path) {
    if (child_ && !co_await child_->Init(L, ctx, base_path)) {
        set_last_error(child_->last_error());
        co_return false;
    }
    co_return true;
}

void Repeat::ReleaseRefs() {
    if (child_) child_->ReleaseRefs();
}
