#include "wait_node.h"

#include "bt_event_queue.h"

WaitNode::WaitNode(uint32_t id, std::string name, int min_ms, int max_ms)
    : Leaf(id, "Wait", std::move(name)),
      rng_(std::random_device{}()) {
    int lo = min_ms < 0 ? 0 : min_ms;
    int hi = max_ms < 0 ? 0 : max_ms;
    if (lo > hi) std::swap(lo, hi);
    min_ms_ = lo;
    max_ms_ = hi;
}

NodeStatus WaitNode::Tick(Blackboard& /*bb*/, BtEventQueue& events) {
    if (!resolved_) {
        resolved_ = true;
        cur_ms_ = RollIntInRange(min_ms_, max_ms_, rng_);
        start_ms_ = events.now_ms();  // per-tick cached time
    }

    if (cur_ms_ <= 0) {
        return NodeStatus::kSuccess;
    }

    if (events.now_ms() - start_ms_ >= cur_ms_) {
        return NodeStatus::kSuccess;
    }
    return NodeStatus::kRunning;
}

void WaitNode::Reset() {
    resolved_ = false;
    Leaf::Reset();
}
