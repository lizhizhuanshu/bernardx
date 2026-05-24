#pragma once

#include <string>

#include "single_child_node.h"

class RetryUntilSuccessful : public SingleChildNode {
public:
    static constexpr int kInfinite = -1;

    RetryUntilSuccessful(uint32_t id, std::string name, int max_attempts,
                         std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    int max_attempts_;
    int attempt_count_ = 0;
};
