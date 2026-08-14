#include "retry.h"

Retry::Retry(uint32_t id, std::string name, int max_count,
             std::unique_ptr<Node> child)
    : SingleChildNode(id, "Retry", std::move(name), std::move(child)),
      max_count_(max_count) {}

NodeStatus Retry::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!child_) {
        set_last_error("no child node");
        return NodeStatus::kFailure;
    }

    auto status = child_->TickAndRecord(bb, events);
    if (status == NodeStatus::kSuccess) {
        return NodeStatus::kSuccess;
    }
    if (status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
    }

    ++attempt_count_;
    if (max_count_ != kInfinite && attempt_count_ >= max_count_) {
        set_last_error(child_->last_error());
        return NodeStatus::kFailure;
    }

    child_->Reset();
    return NodeStatus::kRunning;
}

void Retry::Reset() {
    attempt_count_ = 0;
    SingleChildNode::Reset();
}
