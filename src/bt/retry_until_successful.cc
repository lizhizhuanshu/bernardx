#include "retry_until_successful.h"

RetryUntilSuccessful::RetryUntilSuccessful(uint32_t id, std::string name,
                                           int max_attempts,
                                           std::unique_ptr<Node> child)
    : Node(id, "RetryUntilSuccessful", std::move(name)),
      child_(std::move(child)),
      max_attempts_(max_attempts) {
    if (child_) child_->set_parent(this);
}

NodeStatus RetryUntilSuccessful::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!child_) {
        set_last_error("no child node");
        return NodeStatus::kFailure;
    }

    auto status = child_->Tick(bb, events);
    if (status == NodeStatus::kSuccess) {
        return NodeStatus::kSuccess;
    }
    if (status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
    }

    // Child failed: retry
    ++attempt_count_;
    if (max_attempts_ != kInfinite && attempt_count_ >= max_attempts_) {
        set_last_error(child_->last_error());
        return NodeStatus::kFailure;
    }

    child_->Reset();
    return NodeStatus::kRunning;
}

void RetryUntilSuccessful::Reset() {
    attempt_count_ = 0;
    if (child_) child_->Reset();
    Node::Reset();
}

void RetryUntilSuccessful::OnAborted() {
    if (child_) child_->OnAborted();
    Node::OnAborted();
}
