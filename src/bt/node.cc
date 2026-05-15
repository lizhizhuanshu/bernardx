#include "node.h"

Node::Node(uint32_t id, std::string type, std::string name)
    : id_(id), type_(std::move(type)), name_(std::move(name)) {}

void Node::Reset() {
    prev_decorator_results_.clear();
}

void Node::OnAborted() {
    prev_decorator_results_.clear();
}

void Node::AddDecorator(std::unique_ptr<Decorator> dec) {
    decorators_.push_back(std::move(dec));
}
