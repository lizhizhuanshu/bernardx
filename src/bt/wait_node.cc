#include "wait_node.h"

WaitNode::WaitNode(uint32_t id, std::string name, int wait_ms)
    : Leaf(id, "Wait", std::move(name)),
      wait_ms_(wait_ms) {}

NodeStatus WaitNode::Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) {
    if (wait_ms_ <= 0) {
        return NodeStatus::kSuccess;
    }

    if (!started_) {
        started_ = true;
        start_time_ = std::chrono::steady_clock::now();
        return NodeStatus::kRunning;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_).count();

    if (elapsed >= wait_ms_) {
        return NodeStatus::kSuccess;
    }
    return NodeStatus::kRunning;
}

void WaitNode::Reset() {
    started_ = false;
    Leaf::Reset();
}
