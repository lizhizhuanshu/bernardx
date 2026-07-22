#include "random_selector.h"

RandomSelector::RandomSelector(uint32_t id, std::string name)
    : Composite(id, "RandomSelector", std::move(name)) {}

NodeStatus RandomSelector::Tick(Blackboard& bb, BtEventQueue& events) {
    return TickSequential(bb, events, SeqPolicy::Selector, &shuffled_tracker_);
}

void RandomSelector::Reset() {
    shuffled_tracker_.Reset();
    Composite::Reset();
}
