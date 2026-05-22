#pragma once

#include <memory>
#include <string>

#include "node.h"

class Repeat : public Node {
public:
    static constexpr int kInfinite = -1;

    Repeat(uint32_t id, std::string name, int count,
           std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

    Node* child() const { return child_.get(); }

private:
    std::unique_ptr<Node> child_;
    int max_count_;
    int current_count_ = 0;
};
