#pragma once

#include <chrono>
#include <random>
#include <string>

#include "bt_utils.h"
#include "leaf.h"

// Waits a wall-clock duration before succeeding. The wait is a uniformly
// random value in the inclusive [min_ms, max_ms] range (0 = succeed
// immediately), resolved on the first tick of each run and re-rolled on Reset.
// So a range adds only run-to-run jitter (fixed within one run) — the same
// contract Pipeline uses for its $timeout/$retry ranges. lo==hi collapses to a
// fixed wait (how JSON `min_timeout` without `max_timeout` is expressed).
class WaitNode : public Leaf {
public:
    WaitNode(uint32_t id, std::string name, int min_ms, int max_ms);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    int min_ms_;
    int max_ms_;
    int cur_ms_ = 0;       // resolved per run
    bool resolved_ = false;
    std::chrono::steady_clock::time_point start_time_;
    std::mt19937 rng_;
};
