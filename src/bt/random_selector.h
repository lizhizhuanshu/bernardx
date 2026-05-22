#pragma once

#include "composite.h"

class RandomSelector : public Composite {
public:
    RandomSelector(uint32_t id, std::string name);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    void ShuffleIndices();
    std::vector<size_t> order_;
    bool shuffled_ = false;
};
