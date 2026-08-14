#include "repeat.h"

#include "bt_utils.h"

Repeat::Repeat(uint32_t id, std::string name, int count,
               std::unique_ptr<Node> child)
    : SingleChildNode(id, "Repeat", std::move(name), std::move(child)),
      max_count_(count) {}

Repeat::Repeat(uint32_t id, std::string name, int lo, int hi,
               std::unique_ptr<Node> child)
    : SingleChildNode(id, "Repeat", std::move(name), std::move(child)),
      max_count_(lo),
      lo_(lo),
      hi_(hi),
      resolved_(false) {}

void Repeat::ResolveCount() {
    resolved_ = true;
    if (lo_ <= hi_) {  // range mode: roll once per run
        max_count_ = RollIntInRange(lo_, hi_, rng_);
    }
}

NodeStatus Repeat::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!child_) {
        set_last_error("no child node");
        return NodeStatus::kFailure;
    }

    if (!resolved_) ResolveCount();

    if (max_count_ == kInfinite) {
        auto status = child_->TickAndRecord(bb, events);
        if (status == NodeStatus::kFailure) {
            set_last_error(child_->last_error());
            return NodeStatus::kFailure;
        }
        if (status == NodeStatus::kRunning) {
            return NodeStatus::kRunning;
        }
        child_->Reset();
        return NodeStatus::kRunning;
    }

    if (current_count_ >= max_count_) {
        return NodeStatus::kSuccess;
    }

    auto status = child_->TickAndRecord(bb, events);
    if (status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
    }
    if (status == NodeStatus::kFailure) {
        set_last_error(child_->last_error());
        return NodeStatus::kFailure;
    }

    ++current_count_;
    child_->Reset();

    if (current_count_ >= max_count_) {
        return NodeStatus::kSuccess;
    }
    return NodeStatus::kRunning;
}

void Repeat::Reset() {
    current_count_ = 0;
    resolved_ = false;  // re-roll the range on the next run
    SingleChildNode::Reset();
}
