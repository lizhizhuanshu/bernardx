#pragma once

#include <string>

#include "single_child_node.h"

// Forces the wrapped child's result to Failure once it finishes. Running is
// passed through unchanged (an in-progress child cannot be forced).
class ForceFailure : public SingleChildNode {
public:
    ForceFailure(uint32_t id, std::string name, std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
};
