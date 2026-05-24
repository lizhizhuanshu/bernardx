#include "sequence.h"

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"

Sequence::Sequence(uint32_t id, std::string name)
    : Composite(id, "Sequence", std::move(name)) {}

NodeStatus Sequence::Tick(Blackboard& bb, BtEventQueue& events) {
    for (size_t i = current_child_index_; i < children_.size(); ++i) {
        auto status = children_[i]->Tick(bb, events);
        switch (status) {
            case NodeStatus::kRunning:
                current_child_index_ = i;
                return NodeStatus::kRunning;
            case NodeStatus::kFailure:
                set_last_error(children_[i]->last_error());
                current_child_index_ = 0;
                return NodeStatus::kFailure;
            case NodeStatus::kSuccess:
                continue;
        }
    }
    current_child_index_ = 0;
    return NodeStatus::kSuccess;
}
