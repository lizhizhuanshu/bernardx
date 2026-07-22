#include "selector.h"

Selector::Selector(uint32_t id, std::string name)
    : Composite(id, "Selector", std::move(name)) {}

NodeStatus Selector::Tick(Blackboard& bb, BtEventQueue& events) {
    return TickSequential(bb, events, SeqPolicy::Selector);
}
