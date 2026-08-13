#pragma once

#include <string>

#include "leaf.h"

// Constant-result leaf nodes. `Success` always ticks Success, `Failure` always
// ticks Failure. Stateless on their own — their usefulness comes from carrying
// a guard `condition` (set via Node::SetCondition): a composite gates entry on
// the condition, and only when it holds does the constant result fire. Typical
// use is a Selector's "already done → succeed" branch:
//   { "type":"Success", "condition":{ "type":"Script", "source":"conds/done.lua" } }
class SuccessNode : public Leaf {
public:
    SuccessNode(uint32_t id, std::string name)
        : Leaf(id, "Success", std::move(name)) {}

    NodeStatus Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) override {
        return NodeStatus::kSuccess;
    }
};

class FailureNode : public Leaf {
public:
    FailureNode(uint32_t id, std::string name)
        : Leaf(id, "Failure", std::move(name)) {}

    NodeStatus Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) override {
        return NodeStatus::kFailure;
    }
};
