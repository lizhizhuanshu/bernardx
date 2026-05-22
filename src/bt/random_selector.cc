#include "random_selector.h"

#include <algorithm>
#include <random>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"

RandomSelector::RandomSelector(uint32_t id, std::string name)
    : Composite(id, "RandomSelector", std::move(name)) {}

NodeStatus RandomSelector::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!shuffled_) {
        ShuffleIndices();
        shuffled_ = true;
    }

    for (size_t i = current_child_index_; i < order_.size(); ++i) {
        auto status = children_[order_[i]]->Tick(bb, events);
        switch (status) {
            case NodeStatus::kRunning:
                current_child_index_ = i;
                return NodeStatus::kRunning;
            case NodeStatus::kSuccess:
                current_child_index_ = 0;
                return NodeStatus::kSuccess;
            case NodeStatus::kFailure:
                continue;
        }
    }
    current_child_index_ = 0;
    return NodeStatus::kFailure;
}

void RandomSelector::Reset() {
    shuffled_ = false;
    order_.clear();
    Composite::Reset();
}

void RandomSelector::ShuffleIndices() {
    order_.resize(children_.size());
    for (size_t i = 0; i < order_.size(); ++i) {
        order_[i] = i;
    }
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(order_.begin(), order_.end(), g);
}
