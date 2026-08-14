#pragma once

#include <random>
#include <string>
#include <vector>

#include "composite.h"

// Pipeline is the core composite for software-automation flows (judge page →
// act → judge target page → act). It is a tick-driven state machine with no
// internal sleeping: each Tick it records state and decides what to do. The
// only wall-clock use is the per-step wait budget below.
//
// Each child is one step: an action node that may carry a guard `condition`
// (Node::SetCondition) plus two Pipeline edge params the node itself ignores:
//   `*timeout` (int or [lo,hi] MS) — wall-clock milliseconds to wait for this
//                               step's condition before timing out
//                               (0 = wait forever);
//   `*retry`   (int or [lo,hi])  — max times to back up and re-run the
//                               previous step's action when this wait times out.
//
// Either edge param may be a single integer (fixed value) or a two-element
// `[lo, hi]` array (uniformly random in the inclusive range). The Pipeline
// rolls ONE value per step, per run: the rolled timeout/retry are resolved
// lazily when the step first enters its wait window, then stay fixed for that
// step until Reset() re-rolls them on the next run. So a range only adds
// run-to-run jitter; within a single run each step behaves as if it had a
// fixed value.
//
// Execution (recorded state: current step, phase, ticks waited, retries used):
//   1. First tick scans children top-to-bottom and starts at the first whose
//      condition already holds (resume entry — "which step am I on?"). If none
//      holds, it starts at step 0 and waits for condition[0].
//   2. Once a step's condition holds, its action runs to completion.
//   3. After action[i] completes, the Pipeline waits for condition[i+1],
//      stamping the wait's start; once `*timeout` ms elapsed: if `*retry`
//      still has budget, back up one step — re-check
//      condition[i]; if it holds, reset + re-run action[i], then resume waiting
//      for condition[i+1] with a fresh tick budget. Up to `$retry` times. If
//      condition[i] doesn't hold on retry, or the budget is exhausted, fail.
class Pipeline : public Composite {
public:
    Pipeline(uint32_t id, std::string name);

    // Register a step with *timeout/*retry each given as an inclusive [lo,hi]
    // range (*timeout in ms). The values are resolved (rolled) lazily per run;
    // lo==hi collapses to a fixed value, and 0 keeps the "wait forever" /
    // "no retry" semantics.
    void AddStep(std::unique_ptr<Node> child, int timeout_lo, int timeout_hi,
                 int retry_lo, int retry_hi);
    // Convenience: fixed values for both params (lo==hi). Kept so existing
    // callers and tests read naturally; hides Composite::AddChild so
    // `steps_`/`retries_used_`/`cur_*` stay parallel to `children_`.
    void AddStep(std::unique_ptr<Node> child, int timeout, int retry) {
        AddStep(std::move(child), timeout, timeout, retry, retry);
    }
    // Default edge params (no timeout, no retry).
    void AddChild(std::unique_ptr<Node> child) { AddStep(std::move(child), 0, 0); }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

private:
    enum class Phase { kWait, kAct };

    // Called when the wait for the current step's condition exceeds its
    // `*timeout` ms budget. Backs up to the previous step or fails.
    NodeStatus OnWaitTimeout(Blackboard& bb, BtEventQueue& events);
    // Lazily roll this step's timeout/retry for the current run on first entry
    // to its wait window; once rolled they stay fixed for the step until Reset.
    void ResolveStepBudgets(size_t i);

    struct Step {
        int timeout_lo = 0;  // *timeout range lower (ms, 0 = wait forever)
        int timeout_hi = 0;  // *timeout range upper (inclusive)
        int retry_lo = 0;    // $retry range lower
        int retry_hi = 0;    // $retry range upper (inclusive)
    };

    std::vector<Step> steps_;         // parallel to children_
    std::vector<int> retries_used_;   // back-up retries used per step
    std::vector<int> cur_timeout_;    // resolved timeout per step (-1 = unrolled this run)
    std::vector<int> cur_retry_;      // resolved retry per step (-1 = unrolled this run)
    std::mt19937 rng_;                // resolves *timeout/*retry ranges per run
    int64_t wait_start_ms_ = 0;  // tick-time (cached) when the current wait window began
    bool started_ = false;           // scan phase complete
    Phase phase_ = Phase::kWait;
};
