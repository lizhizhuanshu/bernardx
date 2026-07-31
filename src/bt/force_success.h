#pragma once

#include <string>

#include "single_child_node.h"

// Forces the wrapped child's result to Success once it finishes. Running is
// passed through unchanged (an in-progress child cannot be forced).
class ForceSuccess : public SingleChildNode {
public:
    ForceSuccess(uint32_t id, std::string name, std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
};
