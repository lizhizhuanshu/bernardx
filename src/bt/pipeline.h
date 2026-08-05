#pragma once

#include <string>
#include <vector>

#include "composite.h"

// Pipeline is the core composite for software-automation flows (judge page →
// act → judge target page → act). It is a pure tick-driven state machine: no
// internal sleeping, no wall-clock — it only records state and, on each Tick,
// decides what to do based on that state.
//
// Each child is one step: an action node that may carry a guard `condition`
// (Node::SetCondition) plus two Pipeline edge params the node itself ignores:
//   `$timeout` (integer TICKS) — how many ticks to wait for this step's
//                               condition before timing out (0 = wait forever);
//   `$retry`   (integer)       — max times to back up and re-run the previous
//                               step's action when this wait times out.
// Real time per tick = bt.exec's `interval_ms`, so `$timeout` ticks × interval
// is the wall-clock budget in production.
//
// Execution (recorded state: current step, phase, ticks waited, retries used):
//   1. First tick scans children top-to-bottom and starts at the first whose
//      condition already holds (resume entry — "which step am I on?"). If none
//      holds, it starts at step 0 and waits for condition[0].
//   2. Once a step's condition holds, its action runs to completion.
//   3. After action[i] completes, the Pipeline waits for condition[i+1],
//      counting one tick per evaluation that doesn't succeed. After `$timeout`
//      ticks: if `$retry` still has budget, back up one step — re-check
//      condition[i]; if it holds, reset + re-run action[i], then resume waiting
//      for condition[i+1] with a fresh tick budget. Up to `$retry` times. If
//      condition[i] doesn't hold on retry, or the budget is exhausted, fail.
class Pipeline : public Composite {
public:
    Pipeline(uint32_t id, std::string name);

    // Register a step. `timeout` ($timeout, in ticks, 0 = wait forever) bounds
    // the wait for THIS step's condition; `retry` ($retry) is the max number of
    // back-up re-runs of the previous step's action when this wait times out.
    void AddStep(std::unique_ptr<Node> child, int timeout, int retry);
    // Convenience: add a step with default edge params (no timeout, no retry).
    // Hides Composite::AddChild so `steps_`/`retries_used_` stay parallel to
    // `children_`.
    void AddChild(std::unique_ptr<Node> child) { AddStep(std::move(child), 0, 0); }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

private:
    enum class Phase { kWait, kAct };

    // Called when the wait for the current step's condition exceeds its
    // `$timeout` tick budget. Backs up to the previous step or fails.
    NodeStatus OnWaitTimeout(Blackboard& bb, BtEventQueue& events);

    struct Step {
        int timeout = 0;  // $timeout in ticks (0 = wait forever)
        int retry = 0;    // $retry
    };

    std::vector<Step> steps_;        // parallel to children_
    std::vector<int> retries_used_;  // back-up retries used per step
    int wait_ticks_ = 0;             // ticks spent waiting in the current window
    bool started_ = false;           // scan phase complete
    Phase phase_ = Phase::kWait;
};
