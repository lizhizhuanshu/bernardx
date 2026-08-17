#include "retry.h"

#include "bt_event_queue.h"
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

Retry::Retry(uint32_t id, std::string name, int max_count,
             int interval_lo_ms, int interval_hi_ms,
             std::unique_ptr<Node> child)
    : SingleChildNode(id, "Retry", std::move(name), std::move(child)),
      max_count_(max_count),
      interval_lo_ms_(interval_lo_ms < 0 ? 0 : interval_lo_ms),
      interval_hi_ms_(interval_hi_ms < interval_lo_ms_
                          ? interval_lo_ms_
                          : interval_hi_ms) {}

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

    if (!resolved_) {
        ResolveCount();
        cur_interval_ms_ = RollIntInRange(interval_lo_ms_, interval_hi_ms_, rng_);
    }

    // Inside a post-failure interval: hold Running until it elapses. The
    // child is not re-ticked (it was Reset below when the wait began).
    if (waiting_) {
        if (events.now_ms() - wait_until_ms_ < cur_interval_ms_) {
            return NodeStatus::kRunning;
        }
        waiting_ = false;
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
    if (cur_interval_ms_ > 0) {  // space out the next attempt
        waiting_ = true;
        wait_until_ms_ = events.now_ms();
    }
    return NodeStatus::kRunning;
}

void Retry::Reset() {
    attempt_count_ = 0;
    resolved_ = false;  // re-roll the range on the next run
    waiting_ = false;
    SingleChildNode::Reset();
}
