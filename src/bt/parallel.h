#pragma once

#include "composite.h"

class Parallel : public Composite {
public:
    enum class Policy { kRequireAll, kRequireOne };

    Parallel(uint32_t id, std::string name,
             Policy success = Policy::kRequireAll,
             Policy failure = Policy::kRequireOne);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;

private:
    Policy success_policy_;
    Policy failure_policy_;
};
