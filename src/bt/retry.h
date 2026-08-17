#pragma once

#include <random>
#include <string>

#include "single_child_node.h"

// Retries its child until Success. The attempt cap is either a fixed number
// (`max_count: 3`, `-1` = infinite) or a [lo, hi] range (`max_count: [1, 3]`)
// — a range is rolled once per run (first tick after Reset) and stays fixed
// within that run, the same contract Wait/Pipeline use for their ranges.
//
// `interval` (ms) optionally spaces out retry attempts: after a failed
// attempt the Retry returns Running WITHOUT re-ticking the child until
// `interval` ms of wall clock have passed. Scalar or [lo, hi] range under the
// same roll-once-per-run contract. 0/absent = retry immediately (legacy
// behavior).
class Retry : public SingleChildNode {
public:
    static constexpr int kInfinite = -1;

    // Fixed cap (kInfinite = retry forever). interval_ms <= 0 = no wait.
    Retry(uint32_t id, std::string name, int max_count,
          std::unique_ptr<Node> child);

    // Uniformly random cap in [lo, hi], re-rolled per run.
    Retry(uint32_t id, std::string name, int lo, int hi,
          std::unique_ptr<Node> child);

    // Cap as above plus a post-failure wait: fixed interval_ms, or a
    // uniformly random one in [interval_lo_ms, interval_hi_ms] re-rolled per
    // run (lo==hi collapses to a fixed interval).
    Retry(uint32_t id, std::string name, int max_count,
          int interval_lo_ms, int interval_hi_ms,
          std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    void ResolveCount();

    int max_count_;          // resolved cap for the current run
    int lo_ = 0, hi_ = -1;   // hi_ < lo_ -> fixed mode (no roll)
    bool resolved_ = false;  // range rolled for this run?
    int attempt_count_ = 0;

    // Post-failure wait (ms), same scalar-or-range contract as max_count_.
    int interval_lo_ms_ = 0, interval_hi_ms_ = 0;  // 0,0 = no wait
    int cur_interval_ms_ = 0;   // resolved per run
    int64_t wait_until_ms_ = 0;  // tick-time (cached) before which the child is not re-ticked
    bool waiting_ = false;       // inside a post-failure interval?
    std::mt19937 rng_{std::random_device{}()};
};
