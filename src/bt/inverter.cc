#include "inverter.h"

Inverter::Inverter(uint32_t id, std::string name, std::unique_ptr<Node> child)
    : SingleChildNode(id, "Inverter", std::move(name), std::move(child)) {}

NodeStatus Inverter::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!child_) {
        set_last_error("no child node");
        return NodeStatus::kFailure;
    }
    if (!child_->CheckDecorators(bb)) {
        set_last_error("child decorator condition not met");
        return NodeStatus::kFailure;
    }
    auto status = child_->TickAndRecord(bb, events);
    if (status == NodeStatus::kRunning) {
        return NodeStatus::kRunning;
    }
    return (status == NodeStatus::kSuccess) ? NodeStatus::kFailure : NodeStatus::kSuccess;
}
