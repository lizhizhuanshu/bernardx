#include "force_failure.h"

ForceFailure::ForceFailure(uint32_t id, std::string name, std::unique_ptr<Node> child)
    : SingleChildNode(id, "ForceFailure", std::move(name), std::move(child)) {}

NodeStatus ForceFailure::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!child_) {
        set_last_error("no child node");
        return NodeStatus::kFailure;
    }
    auto status = child_->TickAndRecord(bb, events);
    if (status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
    }
    return NodeStatus::kFailure;
}
