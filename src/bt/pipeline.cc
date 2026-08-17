#include "pipeline.h"

#include <algorithm>

#include "bt_event_queue.h"
#include "bt_utils.h"
#include "blackboard.h"
#include "node.h"
#include "node_condition.h"
#include "types.h"

Pipeline::Pipeline(uint32_t id, std::string name)
    : Composite(id, "Pipeline", std::move(name)),
      rng_(std::random_device{}()) {}

void Pipeline::AddStep(std::unique_ptr<Node> child,
                             std::shared_ptr<NodeCondition> target,
                             int timeout_lo, int timeout_hi,
                             int retry_lo, int retry_hi) {
    Composite::AddChild(std::move(child));  // base: set parent + push children_
    steps_.push_back({std::move(target), timeout_lo, timeout_hi, retry_lo, retry_hi});
    retries_used_.push_back(0);
    cur_timeout_.push_back(-1);  // rolled lazily on first wait this run
    cur_retry_.push_back(-1);
    target_prev_.push_back(NodeStatus::kFailure);  // "not yet observed"
}

async_simple::coro::Lazy<bool> Pipeline::Init(lua_State* L, LuaRuntime* ctx) {
    // Composite::Init inits the action children; the *target conditions are
    // owned by this pipeline (not set on the nodes), so init them here.
    if (!co_await Composite::Init(L, ctx)) co_return false;
    for (auto& step : steps_) {
        if (step.target && !co_await step.target->Init(L, ctx)) {
            set_last_error("Pipeline '" + name() + "': target init failed: " +
                           step.target->last_error());
            co_return false;
        }
    }
    co_return true;
}

void Pipeline::ResolveStepBudgets(size_t i) {
    if (cur_timeout_[i] < 0) {
        cur_timeout_[i] = RollIntInRange(steps_[i].timeout_lo, steps_[i].timeout_hi, rng_);
    }
    if (cur_retry_[i] < 0) {
        cur_retry_[i] = RollIntInRange(steps_[i].retry_lo, steps_[i].retry_hi, rng_);
    }
}

NodeStatus Pipeline::EvalTarget(Blackboard& bb, BtEventQueue& events, size_t i) {
    NodeCondition* t = steps_[i].target.get();
    if (t == nullptr) return NodeStatus::kSuccess;
    NodeStatus raw = t->Eval(bb, events);
    // Record the observed status for reactive-abort flip detection wherever
    // a target is read (scan / wait / Self check): a target that was met and
    // advanced from must be seen as "previously met", even if it flipped
    // again within the same tick.
    if (raw != NodeStatus::kRunning &&
        (t->abort() == AbortMode::kLowerPriority || t->abort() == AbortMode::kBoth)) {
        target_prev_[i] = raw;
    }
    return raw;
}

NodeStatus Pipeline::EffectiveTarget(NodeCondition& t, Blackboard& bb,
                                     BtEventQueue& events) {
    NodeStatus raw = t.Eval(bb, events);
    return (raw == NodeStatus::kRunning) ? t.last_terminal() : raw;
}

bool Pipeline::EvaluateTargetAborts(Blackboard& bb, BtEventQueue& events) {
    // Preempt on an earlier step's target flipping met→unmet (its
    // precondition regressed). Only LowerPriority/Both targets participate;
    // only steps before the current one can preempt (an "earlier" regression
    // while a later step works). The baseline (first observation) is recorded
    // for EVERY such target each tick — including the current step's own —
    // so the met state established while advancing is visible later as a
    // "previous" value. One preemption per tick.
    bool preempted = false;
    for (size_t i = 0; i < steps_.size(); ++i) {
        NodeCondition* t = steps_[i].target.get();
        if (t == nullptr) continue;
        AbortMode am = t->abort();
        if (am != AbortMode::kLowerPriority && am != AbortMode::kBoth) continue;
        NodeStatus eff = EffectiveTarget(*t, bb, events);
        NodeStatus prev = target_prev_[i];
        target_prev_[i] = eff;
        if (preempted) continue;  // one reroute per tick; keep recording
        // First observation only records the baseline (no flip yet). A met→
        // unmet flip on an EARLIER step preempts the running later work back
        // to it.
        if (i < current_child_index_ &&
            prev == NodeStatus::kSuccess && eff != NodeStatus::kSuccess) {
            spdlog::info("Pipeline '{}': step {} target regressed (met→unmet), "
                         "preempting back to redo it",
                         name(), i);
            // Abort whatever later step's work is in flight and re-enter the
            // regressed step: fresh act phase (its action re-runs), fresh
            // budgets. If the target came back by the time it re-runs, the
            // normal scan-forward skips it.
            children_[current_child_index_]->OnAborted();
            current_child_index_ = i;
            children_[i]->Reset();
            retries_used_[i] = 0;
            cur_timeout_[i] = -1;
            cur_retry_[i] = -1;
            phase_ = Phase::kAct;
            preempted = true;
        }
    }
    return preempted;
}

bool Pipeline::ScanToPending(Blackboard& bb, BtEventQueue& events, NodeStatus& out) {
    // Skip every step whose target already holds; park on the first pending
    // one. A Running target needs another tick before its verdict is known.
    // An ABSENT target never reads as met here — without a target there is no
    // way to tell the step is already done, so it is always pending (its
    // action must run; completion is the target, checked in the wait phase).
    for (size_t i = current_child_index_; i < children_.size(); ++i) {
        if (steps_[i].target == nullptr) {  // absent target: always pending
            current_child_index_ = i;
            phase_ = Phase::kAct;
            retries_used_[i] = 0;
            cur_timeout_[i] = -1;  // fresh budget on (re)entry
            cur_retry_[i] = -1;
            out = NodeStatus::kRunning;
            return true;
        }
        NodeStatus s = EvalTarget(bb, events, i);
        if (s == NodeStatus::kRunning) {
            // Verdict unknown (an async target mid-eval): STAY in the scan
            // phase - the next tick re-scans from here. This must not fall
            // into kWait: that phase assumes this step's action already ran,
            // and the scan never parked on a pending step.
            phase_ = Phase::kScan;
            out = NodeStatus::kRunning;  // re-scan next tick
            return false;
        }
        if (s != NodeStatus::kSuccess) {  // pending: this step's work to do
            current_child_index_ = i;
            phase_ = Phase::kAct;
            retries_used_[i] = 0;
            cur_timeout_[i] = -1;  // fresh budget on (re)entry
            cur_retry_[i] = -1;
            out = NodeStatus::kRunning;
            return true;
        }
        // Target met → step already done → skip.
    }
    out = NodeStatus::kSuccess;  // every target holds: pipeline complete
    return false;
}

NodeStatus Pipeline::Tick(Blackboard& bb, BtEventQueue& events) {
    const size_t n = children_.size();
    if (n == 0 || current_child_index_ >= n) {
        return NodeStatus::kSuccess;  // empty or already complete
    }

    // --- Initial scan folded into the steady-state loop below: Scan -> Act
    // -> Wait per step. A scan that hits an async (Running) target returns
    // without parking; the next tick re-enters the scan phase.
    if (started_ && EvaluateTargetAborts(bb, events)) {
        return NodeStatus::kRunning;  // preempted; run the redone step next tick
    }
    started_ = true;

    // --- Steady state. A target that just became met or an action that just
    // completed fast-forwards within the same tick.
    while (true) {
        if (phase_ == Phase::kScan) {
            NodeStatus out;
            if (!ScanToPending(bb, events, out)) return out;
            // parked on a pending step; fall through to kAct this tick
        }

        if (phase_ == Phase::kAct) {
            // Reactive *target Self abort: while this step's action runs, the
            // target becoming met aborts the action and advances at once (the
            // goal is already achieved — skip the remaining work).
            NodeCondition* t = steps_[current_child_index_].target.get();
            if (t != nullptr &&
                (t->abort() == AbortMode::kSelf || t->abort() == AbortMode::kBoth) &&
                EffectiveTarget(*t, bb, events) == NodeStatus::kSuccess) {
                spdlog::info("Pipeline '{}': step {} target met mid-action, "
                             "aborting the rest of the action",
                             name(), current_child_index_);
                children_[current_child_index_]->OnAborted();
                phase_ = Phase::kWait;  // the wait below advances this tick
                wait_start_ms_ = events.now_ms();
                ResolveStepBudgets(current_child_index_);
                continue;  // fall into the wait with the target already met
            }

            NodeStatus s = children_[current_child_index_]->TickAndRecord(bb, events);
            if (s == NodeStatus::kRunning) return NodeStatus::kRunning;
            if (s == NodeStatus::kFailure) {
                if (!children_[current_child_index_]->last_error().empty()) {
                    set_last_error(children_[current_child_index_]->last_error());
                }
                return NodeStatus::kFailure;
            }
            // Action done → wait for this step's target (absent target: met).
            phase_ = Phase::kWait;
            wait_start_ms_ = events.now_ms();
            ResolveStepBudgets(current_child_index_);
            // fall through to the wait this tick
        }

        // phase == kWait (only entered after this step's action ran):
        // evaluate the current step's target.
        NodeStatus s = EvalTarget(bb, events, current_child_index_);
        if (s == NodeStatus::kSuccess) {
            // Target met -> advance past this step and re-scan (skip any
            // later steps whose targets already hold).
            ++current_child_index_;
            if (current_child_index_ >= n) return NodeStatus::kSuccess;
            phase_ = Phase::kScan;
            continue;  // scan (and possibly act) this tick
        }
        // Running or Failure → target not yet met; check the wall-clock budget.
        int timeout = cur_timeout_[current_child_index_];
        if (timeout > 0 && events.now_ms() - wait_start_ms_ >= timeout) {
            return OnWaitTimeout();
        }
        return NodeStatus::kRunning;
    }
}

NodeStatus Pipeline::OnWaitTimeout() {
    const size_t i = current_child_index_;
    // Retry budget exhausted (cur_retry_[i] is resolved: we entered this wait
    // window first) → the step's target never came.
    if (retries_used_[i] >= cur_retry_[i]) {
        set_last_error("Pipeline '" + name() + "': step " + std::to_string(i) +
                       " target not met after action + retries");
        return NodeStatus::kFailure;
    }

    // Re-run this step's action, then wait for its target again (with a
    // fresh ms budget) on subsequent ticks.
    ++retries_used_[i];
    children_[i]->Reset();
    phase_ = Phase::kAct;
    return NodeStatus::kRunning;  // tick the re-run action on the next tick
}

void Pipeline::Reset() {
    started_ = false;
    phase_ = Phase::kScan;
    wait_start_ms_ = 0;
    std::fill(retries_used_.begin(), retries_used_.end(), 0);
    std::fill(cur_timeout_.begin(), cur_timeout_.end(), -1);  // re-roll next run
    std::fill(cur_retry_.begin(), cur_retry_.end(), -1);
    std::fill(target_prev_.begin(), target_prev_.end(), NodeStatus::kFailure);
    for (auto& step : steps_) {
        if (step.target) step.target->Reset();
    }
    Composite::Reset();  // current_child_index_ = 0; reset children
}

void Pipeline::OnAborted() {
    started_ = false;
    phase_ = Phase::kScan;
    wait_start_ms_ = 0;
    std::fill(cur_timeout_.begin(), cur_timeout_.end(), -1);
    std::fill(cur_retry_.begin(), cur_retry_.end(), -1);
    std::fill(target_prev_.begin(), target_prev_.end(), NodeStatus::kFailure);
    Composite::OnAborted();
}
