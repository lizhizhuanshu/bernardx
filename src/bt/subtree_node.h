#pragma once

#include <memory>
#include <string>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"
#include "node.h"

class SubtreeNode : public Node {
public:
    SubtreeNode(uint32_t id, std::string name, std::string subtree_name,
                std::unique_ptr<Node> subtree_root)
        : Node(id, "Subtree", std::move(name)),
          subtree_name_(std::move(subtree_name)),
          subtree_root_(std::move(subtree_root)) {
        if (subtree_root_) {
            subtree_root_->set_parent(this);
        }
    }

    const std::string& subtree_name() const { return subtree_name_; }
    Node* subtree_root() const { return subtree_root_.get(); }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override {
        if (!subtree_root_) {
            set_last_error("no subtree root");
            return NodeStatus::kFailure;
        }
        auto status = subtree_root_->Tick(bb, events);
        if (status == NodeStatus::kFailure && !subtree_root_->last_error().empty()) {
            set_last_error(subtree_root_->last_error());
        }
        return status;
    }

    void Reset() override {
        if (subtree_root_) subtree_root_->Reset();
        Node::Reset();
    }

    void OnAborted() override {
        if (subtree_root_) subtree_root_->OnAborted();
        Node::OnAborted();
    }

    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx,
                                         const std::string& base_path) override {
        if (subtree_root_ && !co_await subtree_root_->Init(L, ctx, base_path)) {
            set_last_error(subtree_root_->last_error());
            co_return false;
        }
        co_return true;
    }

private:
    std::string subtree_name_;
    std::unique_ptr<Node> subtree_root_;
};
