#pragma once

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "composite.h"
#include "node_condition.h"

// Pipeline is the core composite for software-automation flows: each
// step is an action plus a `*target` condition describing the step's TARGET
// state ("this step is done when ..."). It is a tick-driven state machine
// with no internal sleeping; the only wall-clock use is the per-step
// `*timeout` budget below.
//
// Inversion vs the historical condition-scan semantics: a step's action runs
// while its target is NOT met. The target no longer selects an entry point —
// it gates skipping:
//   1. On entry (and after each advance), scan children top-to-bottom and
//      SKIP every step whose target already holds (the work is done). Start
//      at the first step whose target is not met — its action runs. If every
//      target holds, the whole pipeline succeeds immediately.
//   2. The current step's action runs (possibly several ticks). When the
//      action completes, WAIT for its target to become met (the target may
//      already have flipped mid-run — then advance at once).
//   3. Target met → advance to the next step (re-scan: later steps whose
//      targets already hold are skipped the same way).
//   4. Waiting exceeds `*timeout` ms → re-run the action (Reset + tick
//      again), up to `*retry` times; still unmet after the budget is spent
//      → pipeline failure.
//
// Each child carries three Pipeline edge params (the `*` prefix marks
// them as pipeline edge params the node itself ignores):
//   `*target`  (condition object) — the step's target state; absent = the
//                                step is never "already done": run the
//                                action; completion IS the target.
//   `*timeout` (int or [lo,hi] MS) — wall-clock milliseconds to wait for
//                                this step's target after an action run
//                                (0 = wait forever);
//   `*retry`   (int or [lo,hi])  — max times to re-run this step's action
//                                when its target wait times out.
//
// `*timeout`/`*retry` may be a single integer (fixed) or a two-element
// `[lo, hi]` array (uniformly random, inclusive). The pipeline rolls ONE
// value per step, per run: resolved lazily when the step first enters its
// wait window, then fixed until Reset() re-rolls on the next run. So a range
// only adds run-to-run jitter.
//
// A child's plain `condition` (if any) keeps its usual node-guard meaning
// and is NOT the target.
//
// The `*target` object may also carry an `abort` field ("Self" /
// "LowerPriority" / "Both", default absent) enabling reactive interruption,
// mirrored from guard semantics — for a target, BECOMING MET is the event:
//   Self          — the step's own action is aborted the moment its target
//                   becomes met (the goal is achieved; skip remaining work);
//   LowerPriority — an earlier step's target flipping met→unmet preempts
//                   whatever later step is running and jumps back to redo
//                   the regressed precondition step.
class Pipeline : public Composite {
public:
    Pipeline(uint32_t id, std::string name);

    // Register a step with its target condition plus *timeout/*retry each
    // given as an inclusive [lo,hi] range (*timeout in ms). The values are
    // resolved (rolled) lazily per run; lo==hi collapses to a fixed value,
    // and 0 keeps the "wait forever" / "no retry" semantics.
    void AddStep(std::unique_ptr<Node> child,
                 std::shared_ptr<NodeCondition> target,
                 int timeout_lo, int timeout_hi, int retry_lo, int retry_hi);
    // Convenience: fixed values for both params (lo==hi). Hides
    // Composite::AddChild so `steps_`/`retries_used_`/`cur_*` stay parallel
    // to `children_`.
    void AddStep(std::unique_ptr<Node> child,
                 std::shared_ptr<NodeCondition> target, int timeout, int retry) {
        AddStep(std::move(child), std::move(target), timeout, timeout, retry, retry);
    }
    // Default edge params (no timeout, no retry). Hides the non-virtual
    // Composite::AddChild (same as the other AddStep overloads).
    void AddChild(std::unique_ptr<Node> child) {
        AddStep(std::move(child), nullptr, 0, 0);
    }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;

private:
    enum class Phase { kScan, kAct, kWait };

    // Evaluate step i's target: absent target counts as met.
    NodeStatus EvalTarget(Blackboard& bb, BtEventQueue& events, size_t i);
    // Called when the wait for the current step's target exceeds its
    // `*timeout` ms budget. Re-runs the action or fails.
    NodeStatus OnWaitTimeout();
    // Lazily roll this step's timeout/retry for the current run on first
    // entry to its wait window; once rolled they stay fixed until Reset.
    void ResolveStepBudgets(size_t i);
    // Skip steps whose targets are already met. Returns false when the scan
    // hit a Running target (out=Running: re-scan next tick) or every target
    // is met (out=Success); true when parked on a pending step (kAct).
    bool ScanToPending(Blackboard& bb, BtEventQueue& events, NodeStatus& out);
    // Reactive interruption on *target (the `abort` field, same values as a
    // guard condition but mirrored — for a target, BECOMING MET is the event):
    //   Self           — while this step's action runs, the target becoming
    //                    met aborts the action and advances at once (the work
    //                    is done; skip the rest of it).
    //   LowerPriority  — while a LATER step runs, an earlier step's target
    //                    flipping met→unmet preempts the pipeline back to that
    //                    earlier step (its precondition regressed: redo it).
    // Returns true when a preemption rerouted the pipeline this tick.
    bool EvaluateTargetAborts(Blackboard& bb, BtEventQueue& events);
    // Effective target status with stale-while-running (an async target
    // mid-re-eval counts as its last terminal result).
    NodeStatus EffectiveTarget(NodeCondition& t, Blackboard& bb, BtEventQueue& events);

    struct Step {
        std::shared_ptr<NodeCondition> target;  // *target; null = action-completion is the target
        int timeout_lo = 0;  // *timeout range lower (ms, 0 = wait forever)
        int timeout_hi = 0;  // *timeout range upper (inclusive)
        int retry_lo = 0;    // *retry range lower
        int retry_hi = 0;    // *retry range upper (inclusive)
    };

    std::vector<Step> steps_;        // parallel to children_
    std::vector<int> retries_used_;  // action re-runs used per step
    std::vector<int> cur_timeout_;   // resolved timeout per step (-1 = unrolled this run)
    std::vector<int> cur_retry_;     // resolved retry per step (-1 = unrolled this run)
    // Last observed effective status per step target (for met→unmet flip
    // detection); empty until a LowerPriority/Both target is first observed.
    std::vector<NodeStatus> target_prev_;
    std::mt19937 rng_;               // resolves *timeout/*retry ranges per run
    int64_t wait_start_ms_ = 0;  // tick-time (cached) when the current wait window began
    bool started_ = false;           // initial scan phase complete
    Phase phase_ = Phase::kScan;
};
