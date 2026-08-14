#include "retry.h"

#include "bt_utils.h"

Retry::Retry(uint32_t id, std::string name, int max_count,
             std::unique_ptr<Node> child)
    : SingleChildNode(id, "Retry", std::move(name), std::move(child)),
      max_count_(max_count) {}

Retry::Retry(uint32_t id, std::string name, int lo, int hi,
             std::unique_ptr<Node> child)
    : SingleChildNode(id, "Retry", std::move(name), std::move(child)),
      max_count_(lo),
      lo_(lo),
      hi_(hi),
      resolved_(false) {}

void Retry::ResolveCount() {
    resolved_ = true;
    if (lo_ <= hi_) {  // range mode: roll once per run
        max_count_ = RollIntInRange(lo_, hi_, rng_);
    }
}

NodeStatus Retry::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!child_) {
        set_last_error("no child node");
        return NodeStatus::kFailure;
    }

    if (!resolved_) ResolveCount();

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
    resolved_ = false;  // re-roll the range on the next run
    SingleChildNode::Reset();
}
