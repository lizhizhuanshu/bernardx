#include "random_sequence.h"

#include <algorithm>
#include <random>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"

RandomSequence::RandomSequence(uint32_t id, std::string name)
    : Composite(id, "RandomSequence", std::move(name)) {}

NodeStatus RandomSequence::Tick(Blackboard& bb, BtEventQueue& events) {
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
            case NodeStatus::kFailure:
                set_last_error(children_[order_[i]]->last_error());
                current_child_index_ = 0;
                return NodeStatus::kFailure;
            case NodeStatus::kSuccess:
                continue;
        }
    }
    current_child_index_ = 0;
    return NodeStatus::kSuccess;
}

void RandomSequence::Reset() {
    shuffled_ = false;
    order_.clear();
    Composite::Reset();
}

void RandomSequence::ShuffleIndices() {
    order_.resize(children_.size());
    for (size_t i = 0; i < order_.size(); ++i) {
        order_[i] = i;
    }
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(order_.begin(), order_.end(), g);
}
