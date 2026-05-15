#include "composite.h"

Composite::Composite(uint32_t id, std::string type, std::string name)
    : Node(id, std::move(type), std::move(name)) {}

void Composite::AddChild(std::unique_ptr<Node> child) {
    child->set_parent(this);
    children_.push_back(std::move(child));
}

void Composite::Reset() {
    current_child_index_ = 0;
    for (auto& child : children_) {
        child->Reset();
    }
    Node::Reset();
}

void Composite::OnAborted() {
    current_child_index_ = 0;
    for (auto& child : children_) {
        child->OnAborted();
    }
    Node::OnAborted();
}
