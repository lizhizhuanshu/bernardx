#include "selector.h"

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"

Selector::Selector(uint32_t id, std::string name)
    : Composite(id, "Selector", std::move(name)) {}

NodeStatus Selector::Tick(Blackboard& bb, BtEventQueue& events) {
    for (size_t i = current_child_index_; i < children_.size(); ++i) {
        auto& child = children_[i];
        if (!child->CheckDecorators(bb)) {
            continue;
        }
        auto status = child->Tick(bb, events);
        switch (status) {
            case NodeStatus::kRunning:
                current_child_index_ = i;
                return NodeStatus::kRunning;
            case NodeStatus::kSuccess:
                current_child_index_ = 0;
                return NodeStatus::kSuccess;
            case NodeStatus::kFailure:
                if (!child->last_error().empty()) {
                    set_last_error(child->last_error());
                }
                continue;
        }
    }
    current_child_index_ = 0;
    return NodeStatus::kFailure;
}
