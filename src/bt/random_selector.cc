#include "random_selector.h"

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"

RandomSelector::RandomSelector(uint32_t id, std::string name)
    : Composite(id, "RandomSelector", std::move(name)) {}

NodeStatus RandomSelector::Tick(Blackboard& bb, BtEventQueue& events) {
    shuffled_tracker_.EnsureShuffled(children_.size());

    const auto& order = shuffled_tracker_.order();
    for (size_t i = current_child_index_; i < order.size(); ++i) {
        auto status = children_[order[i]]->Tick(bb, events);
        switch (status) {
            case NodeStatus::kRunning:
                current_child_index_ = i;
                return NodeStatus::kRunning;
            case NodeStatus::kSuccess:
                current_child_index_ = 0;
                return NodeStatus::kSuccess;
            case NodeStatus::kFailure:
                set_last_error(children_[order[i]]->last_error());
                continue;
        }
    }
    current_child_index_ = 0;
    return NodeStatus::kFailure;
}

void RandomSelector::Reset() {
    shuffled_tracker_.Reset();
    Composite::Reset();
}
