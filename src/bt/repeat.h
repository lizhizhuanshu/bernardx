#pragma once

#include <string>

#include "single_child_node.h"

class Repeat : public SingleChildNode {
public:
    static constexpr int kInfinite = -1;

    Repeat(uint32_t id, std::string name, int count,
           std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    int max_count_;
    int current_count_ = 0;
};
