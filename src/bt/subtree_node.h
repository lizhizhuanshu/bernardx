#pragma once

#include <memory>
#include <string>

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
        if (!subtree_root_) return NodeStatus::kFailure;
        return subtree_root_->Tick(bb, events);
    }

    void Reset() override {
        if (subtree_root_) subtree_root_->Reset();
        Node::Reset();
    }

    void OnAborted() override {
        if (subtree_root_) subtree_root_->OnAborted();
        Node::OnAborted();
    }

private:
    std::string subtree_name_;
    std::unique_ptr<Node> subtree_root_;
};
