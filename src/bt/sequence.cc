#include "sequence.h"

Sequence::Sequence(uint32_t id, std::string name)
    : Composite(id, "Sequence", std::move(name)) {}

NodeStatus Sequence::Tick(Blackboard& bb, BtEventQueue& events) {
    return TickSequential(bb, events, SeqPolicy::Sequence);
}
