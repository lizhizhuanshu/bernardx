#include "random_sequence.h"

RandomSequence::RandomSequence(uint32_t id, std::string name)
    : Composite(id, "RandomSequence", std::move(name)) {}

NodeStatus RandomSequence::Tick(Blackboard& bb, BtEventQueue& events) {
    return TickSequential(bb, events, SeqPolicy::Sequence, &shuffled_tracker_);
}

void RandomSequence::Reset() {
    shuffled_tracker_.Reset();
    Composite::Reset();
}
