#pragma once

#include <chrono>
#include <string>

#include "leaf.h"

class WaitNode : public Leaf {
public:
    WaitNode(uint32_t id, std::string name, int wait_ms);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    int wait_ms_;
    bool started_ = false;
    std::chrono::steady_clock::time_point start_time_;
};
