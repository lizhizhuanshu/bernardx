#pragma once

#include "composite.h"

class Selector : public Composite {
public:
    Selector(uint32_t id, std::string name);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
};
