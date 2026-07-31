#pragma once

#include <string>

#include "single_child_node.h"

// Inverts the wrapped child's terminal result: Success <-> Failure. Running is
// passed through unchanged (an in-progress child has no result to invert yet).
class Inverter : public SingleChildNode {
public:
    Inverter(uint32_t id, std::string name, std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
};
