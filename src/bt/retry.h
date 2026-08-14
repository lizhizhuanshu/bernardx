#pragma once

#include <random>
#include <string>

#include "single_child_node.h"

// Retries its child until Success. The attempt cap is either a fixed number
// (`max_count: 3`, `-1` = infinite) or a [lo, hi] range (`max_count: [1, 3]`)
// — a range is rolled once per run (first tick after Reset) and stays fixed
// within that run, the same contract Wait/Pipeline use for their ranges.
class Retry : public SingleChildNode {
public:
    static constexpr int kInfinite = -1;

    // Fixed cap (kInfinite = retry forever).
    Retry(uint32_t id, std::string name, int max_count,
          std::unique_ptr<Node> child);

    // Uniformly random cap in [lo, hi], re-rolled per run.
    Retry(uint32_t id, std::string name, int lo, int hi,
          std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    void ResolveCount();

    int max_count_;          // resolved cap for the current run
    int lo_ = 0, hi_ = -1;   // hi_ < lo_ -> fixed mode (no roll)
    bool resolved_ = false;  // range rolled for this run?
    int attempt_count_ = 0;
    std::mt19937 rng_{std::random_device{}()};
};
