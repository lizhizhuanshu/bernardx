#pragma once

#include <random>
#include <string>

#include "single_child_node.h"

// Repeats its child. The count is either a fixed number (`count: 3`, `-1` =
// infinite) or a [lo, hi] range (`count: [2, 5]`) — a range is rolled once
// per run (first tick after Reset) and stays fixed within that run, the same
// contract Wait/Pipeline use for their ranges.
class Repeat : public SingleChildNode {
public:
    static constexpr int kInfinite = -1;

    // Fixed count (kInfinite = repeat forever).
    Repeat(uint32_t id, std::string name, int count,
           std::unique_ptr<Node> child);

    // Uniformly random count in [lo, hi], re-rolled per run.
    Repeat(uint32_t id, std::string name, int lo, int hi,
           std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    void ResolveCount();

    int max_count_;          // resolved count for the current run
    int lo_ = 0, hi_ = -1;   // hi_ < lo_ -> fixed mode (no roll)
    bool resolved_ = false;  // range rolled for this run?
    int current_count_ = 0;
    std::mt19937 rng_{std::random_device{}()};
};
