#pragma once

#include <string>

#include "single_child_node.h"

class Retry : public SingleChildNode {
public:
    static constexpr int kInfinite = -1;

    Retry(uint32_t id, std::string name, int max_count,
          std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    int max_count_;
    int attempt_count_ = 0;
};
