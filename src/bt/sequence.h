#pragma once

#include "composite.h"

class Sequence : public Composite {
public:
    Sequence(uint32_t id, std::string name);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
};
