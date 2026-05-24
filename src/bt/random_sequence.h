#pragma once

#include "bt_utils.h"
#include "composite.h"

class RandomSequence : public Composite {
public:
    RandomSequence(uint32_t id, std::string name);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    ShuffledIndexTracker shuffled_tracker_;
};
