#pragma once

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
}

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
//   2. The current step's action runs (possibly several ticks). The action
//      can return kFailure through TWO distinct mechanisms which the
//      pipeline disambiguates via Node::last_tick_was_guard (set by
//      TickAndRecord's guard short-circuit in node.h, cleared on every
//      action Tick):
//      - action Success → enter kWait for its target (last_action_failed_ =
//        false; the target may have flipped mid-run — then advance at once);
//      - action returned kFailure (last_tick_was_guard == false) → enter the
//        SAME kWait phase with last_action_failed_ = true — a post-failure
//        window sharing the *timeout/*retry budget with target-not-met. The
//        *target is still evaluated EVERY tick (a failed action's side
//        effects may have met it → advance at once). Only a bare step (no
//        *target) with last_action_failed_ reads as not-complete, so it must
//        re-run/fail via OnWaitTimeout;
//      - guard short-circuited kFailure (last_tick_was_guard == true) →
//        enter kWaitGuard. Re-evaluate the guard every tick; on guard
//        Success fall back to kAct and re-tick the action the same tick;
//        on guard still Failure for *timeout ms → pipeline failure. The
//        *retry budget is NOT consumed — a guard loss is an
//        environmental change (page lost, popup appeared), distinct from
//        a step-internal retry; re-running the action is gated by the
//        guard, not by retries;
//      - action running → return Running.
//   3. Target met → advance to the next step (re-scan: later steps whose
//      targets already hold are skipped the same way).
//   4. The single kWait phase re-evaluates the *target every tick, whether
//      the action succeeded or failed. The ONE difference the
//      last_action_failed_ bit makes is bare-step handling: a failed bare
//      action reads as not-complete, so it can't advance on the absent-target
//      shortcut. When the wall-clock `*timeout` exceeds, it goes through
//      OnWaitTimeout(): if `*retry` budget remains, Reset + re-run the action
//      (setting last_action_failed_ afresh on its next exit); if exhausted →
//      pipeline failure.
//      A guard failure is different: kWaitGuard re-evaluates the guard every
//      tick and never goes through OnWaitTimeout — guard Success returns to
//      kAct, sustained guard Failure fails the pipeline directly without
//      touching the *retry counter or Reset()'ing the child.
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
//
// Independently of `abort`, EVERY step's *target doubles as the NEXT step's
// condition, so the retry path (OnWaitTimeout) re-verifies the nearest
// regressed predecessor before re-running a step's action, and redoes it —
// without needing `abort=LowerPriority` (which only adds the per-tick
// reactive preemption).
class Pipeline : public Composite {
public:
    // A `*timeout`/`*retry` value, kept as its RAW description and resolved
    // lazily EACH RUN when the step enters its wait window (so a `$key`
    // blackboard ref is read fresh per run and a [lo,hi] range re-rolls).
    //   kind kFixed -> lo fixed (hi == lo); lo < 0 marks "unset" (defer to
    //                  the pipeline-level default)
    //   kind kRange -> uniform roll in [lo, hi]
    //   kind kBbRef -> read blackboard key `key` (an int, or a {lo,hi} table
    //                  -> a roll for that run); resolution failure logs an
    //                  error and yields 0
    struct EdgeParam {
        enum class Kind { kFixed, kRange, kBbRef };
        Kind kind = Kind::kFixed;
        int lo = 0;
        int hi = 0;
        std::string key;  // kBbRef only
    };

    Pipeline(uint32_t id, std::string name);

    // Register a step with its target condition plus *timeout/*retry specs
    // (*timeout in ms). Both may fall back to the pipeline-level defaults
    // (see SetDefaults) when the step carries none; pass an "unset" EdgeParam
    // (kind kFixed with negative lo) to defer. Values resolve lazily each run
    // when the step enters its wait window.
    void AddStep(std::unique_ptr<Node> child,
                 std::shared_ptr<NodeCondition> target,
                 EdgeParam timeout, EdgeParam retry);
    // Convenience: fixed values for both params. Hides Composite::AddChild
    // so `steps_`/`retries_used_`/`cur_*` stay parallel to `children_`.
    void AddStep(std::unique_ptr<Node> child,
                 std::shared_ptr<NodeCondition> target, int timeout, int retry) {
        AddStep(std::move(child), std::move(target),
                EdgeParam{EdgeParam::Kind::kFixed, timeout, timeout, {}},
                EdgeParam{EdgeParam::Kind::kFixed, retry, retry, {}});
    }
    // Convenience: inclusive [lo,hi] ranges (*timeout in ms; lo==hi fixed).
    void AddStep(std::unique_ptr<Node> child,
                 std::shared_ptr<NodeCondition> target,
                 int timeout_lo, int timeout_hi, int retry_lo, int retry_hi) {
        using EP = EdgeParam;
        EP t = (timeout_lo == timeout_hi)
                   ? EP{EP::Kind::kFixed, timeout_lo, timeout_lo, {}}
                   : EP{EP::Kind::kRange, std::min(timeout_lo, timeout_hi),
                       std::max(timeout_lo, timeout_hi), {}};
        EP r = (retry_lo == retry_hi)
                   ? EP{EP::Kind::kFixed, retry_lo, retry_lo, {}}
                   : EP{EP::Kind::kRange, std::min(retry_lo, retry_hi),
                       std::max(retry_lo, retry_hi), {}};
        AddStep(std::move(child), std::move(target), std::move(t), std::move(r));
    }
    // Default edge params (no timeout, no retry). Hides the non-virtual
    // Composite::AddChild (same as the other AddStep overloads).
    void AddChild(std::unique_ptr<Node> child) {
        AddStep(std::move(child), nullptr, 0, 0);
    }

    // Pipeline-level defaults (`params.timeout`/`params.retry`): used for a
    // step whose own edge param is unset (kind kFixed with lo < 0). Same
    // lazy per-run resolution.
    void SetDefaultTimeout(EdgeParam timeout) { def_timeout_ = std::move(timeout); }
    void SetDefaultRetry(EdgeParam retry) { def_retry_ = std::move(retry); }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;

private:
    enum class Phase {
        kScan,        // skip steps whose target already holds
        kAct,         // tick the current step's action
        kResolve,     // waiting for $key provider reads of *timeout/*retry
        kWait,        // waiting after the action ran — success or failure —
                      // for its *target. Eval'd EVERY tick (a failed action's
                      // side effects may have met it): met → advance; else
                      // OnWaitTimeout on timeout. A bare step (no *target)
                      // whose action FAILED reads as not-complete (see
                      // last_action_failed_), since a failed action never is.
        kWaitGuard,   // waiting after the guard (abort=Self condition)
                      // short-circuited mid-action. Re-evaluates the guard
                      // every tick: guard Success → fall back to kAct and
                      // re-tick the action; guard Failure sustained for
                      // *timeout ms → Pipeline failure. Does NOT consume
                      // the *retry budget (a guard failure is an
                      // environmental change, not a step-internal retry).
    };

    // Evaluate step i's target: absent target counts as met.
    NodeStatus EvalTarget(Blackboard& bb, BtEventQueue& events, size_t i);
    // Called when the wait for the current step's target exceeds its
    // `*timeout` ms budget. Re-verifies the nearest regressed predecessor's
    // target (this step's precondition) first and redoes it if it regressed;
    // otherwise re-runs the action or fails.
    NodeStatus OnWaitTimeout(Blackboard& bb, BtEventQueue& events);
    // Resolve this step's timeout/retry for the CURRENT run when it enters
    // its wait window. Fixed/range params resolve inline; a `$key` ref
    // reads the blackboard — a PROVIDER-served read launches the
    // provider's get as a coroutine and the pipeline parks in kResolve
    // (Running) until both sides complete. Returns false while a resolve
    // is in flight (phase already set); true when cur_*_[i] are final.
    bool ResolveStepBudgets(Blackboard& bb, size_t i);
    // One resolved edge value -> int for this run: an integer, or a
    // {lo,hi} table -> a roll. -1 (logged) = unresolvable value.
    int FinishEdgeParam(const std::optional<LuaValue>& v, const EdgeParam& p,
                        const char* what);
    // Cancel any in-flight edge-param provider resolves.
    void CancelPendingResolves();
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
        EdgeParam timeout;  // *timeout (ms, 0 = wait forever)
        EdgeParam retry;    // *retry (0 = no re-run)
    };

    // One possibly-suspended `$key` provider read for an edge param
    // (mirrors the ScriptNode yielded_co_/has_result_/result_ pattern).
    struct PendingValue {
        lua_State* co = nullptr;  // outstanding coroutine (null = not pending)
        bool done = false;
        std::optional<LuaValue> value;  // finished read (nullopt = miss/error)
        EdgeParam src;                  // the ref being resolved (for logs)
        const char* what = "";          // "*timeout" / "*retry" (for logs)
    };

    std::vector<Step> steps_;        // parallel to children_
    std::vector<int> retries_used_;  // action re-runs used per step
    std::vector<int> cur_timeout_;   // resolved timeout per step (-1 = unresolved this run)
    std::vector<int> cur_retry_;     // resolved retry per step (-1 = unresolved this run)
    EdgeParam def_timeout_;          // params.timeout default for unset steps
    EdgeParam def_retry_;            // params.retry default for unset steps
    // Last observed effective status per step target (for met→unmet flip
    // detection); empty until a LowerPriority/Both target is first observed.
    std::vector<NodeStatus> target_prev_;
    std::mt19937 rng_;               // resolves *timeout/*retry ranges per run
    lua_State* lua_state_ = nullptr; // main state (captured at Init) backing $ref tables
    LuaRuntime* lua_ctx_ = nullptr;  // captured at Init; backs provider resolves
    PendingValue resolve_timeout_;   // kResolve in-flight *timeout read
    PendingValue resolve_retry_;     // kResolve in-flight *retry read
    int64_t wait_start_ms_ = 0;  // tick-time (cached) when the current wait window began
    // For the single kWait phase: did the run that landed us here terminate
    // in Failure? A bare step (no *target) with this true reads as
    // not-complete — a failed action can't advance on the "action completion
    // IS the target" shortcut; it must re-run/fail via OnWaitTimeout. Set at
    // each kAct exit (before budget resolve, so it survives kResolve);
    // it is DATA about the last run, not a control-flow branch.
    bool last_action_failed_ = false;
    bool started_ = false;           // initial scan phase complete
    Phase phase_ = Phase::kScan;
    // Preserve the child's last_error through the post-failure wait window
    // (so OnWaitTimeout can surface it as the pipeline error if the retry
    // budget is exhausted). Cleared on Reset / OnAborted / successful retry.
    std::string pending_action_error_;
};
