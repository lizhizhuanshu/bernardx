#include "wait_node.h"

WaitNode::WaitNode(uint32_t id, std::string name, int min_ms, int max_ms)
    : Leaf(id, "Wait", std::move(name)),
      rng_(std::random_device{}()) {
    int lo = min_ms < 0 ? 0 : min_ms;
    int hi = max_ms < 0 ? 0 : max_ms;
    if (lo > hi) std::swap(lo, hi);
    min_ms_ = lo;
    max_ms_ = hi;
}

NodeStatus WaitNode::Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) {
    if (!resolved_) {
        resolved_ = true;
        cur_ms_ = RollIntInRange(min_ms_, max_ms_, rng_);
        start_time_ = std::chrono::steady_clock::now();
    }

    if (cur_ms_ <= 0) {
        return NodeStatus::kSuccess;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_).count();

    if (elapsed >= cur_ms_) {
        return NodeStatus::kSuccess;
    }
    return NodeStatus::kRunning;
}

void WaitNode::Reset() {
    resolved_ = false;
    Leaf::Reset();
}
