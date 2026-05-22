#pragma once

#include <memory>
#include <string>

#include "node.h"

class RetryUntilSuccessful : public Node {
public:
    static constexpr int kInfinite = -1;

    RetryUntilSuccessful(uint32_t id, std::string name, int max_attempts,
                         std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

    Node* child() const { return child_.get(); }

private:
    std::unique_ptr<Node> child_;
    int max_attempts_;
    int attempt_count_ = 0;
};
