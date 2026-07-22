#pragma once

#include <memory>
#include <string>

#include "single_child_node.h"

// SubtreeNode embeds a parsed subtree as its single child. Lifecycle
// (Reset/OnAborted/Init) is inherited from SingleChildNode; only Tick is
// specialised (null-guarded passthrough with error propagation).
class SubtreeNode : public SingleChildNode {
public:
    SubtreeNode(uint32_t id, std::string name, std::string subtree_name,
                std::unique_ptr<Node> subtree_root)
        : SingleChildNode(id, "Subtree", std::move(name), std::move(subtree_root)),
          subtree_name_(std::move(subtree_name)) {}

    const std::string& subtree_name() const { return subtree_name_; }
    Node* subtree_root() const { return child(); }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override {
        if (!child_) {
            set_last_error("no subtree root");
            return NodeStatus::kFailure;
        }
        if (!child_->CheckDecorators(bb)) {
            return NodeStatus::kFailure;
        }
        auto status = child_->Tick(bb, events);
        if (status == NodeStatus::kFailure && !child_->last_error().empty()) {
            set_last_error(child_->last_error());
        }
        return status;
    }

private:
    std::string subtree_name_;
};
