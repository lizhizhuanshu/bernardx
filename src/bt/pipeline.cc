#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "bt_event_queue.h"
#include "bt_utils.h"
#include "blackboard.h"
#include "node.h"
#include "node_condition.h"
#include "provider_call.h"
#include "types.h"

Pipeline::Pipeline(uint32_t id, std::string name)
    : Composite(id, "Pipeline", std::move(name)),
      rng_(std::random_device{}()) {}

void Pipeline::AddStep(std::unique_ptr<Node> child,
                       std::shared_ptr<NodeCondition> target,
                       EdgeParam timeout, EdgeParam retry) {
    Composite::AddChild(std::move(child));  // base: set parent + push children_
    steps_.push_back({std::move(target), std::move(timeout), std::move(retry)});
    retries_used_.push_back(0);
    cur_timeout_.push_back(-1);  // resolved lazily on each run's first wait
    cur_retry_.push_back(-1);
    target_prev_.push_back(NodeStatus::kFailure);  // "not yet observed"
}

async_simple::coro::Lazy<bool> Pipeline::Init(lua_State* L, LuaRuntime* ctx) {
    // Composite::Init inits the action children; the *target conditions are
    // owned by this pipeline (not set on the nodes), so init them here.
    lua_state_ = L;  // backs $key-ref LuaRefs ({lo,hi} tables) at resolve time
    lua_ctx_ = ctx;  // backs provider get coroutines for $key refs
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

// One resolved edge value -> int for THIS run: an integer fixes it, a
// {lo,hi} table rolls. Returns -1 with an error log when the value can't
// be used (the caller treats -1 as 0 = wait forever / no retry).
int Pipeline::FinishEdgeParam(const std::optional<LuaValue>& v,
                              const EdgeParam& p, const char* what) {
    if (!v.has_value()) {
        spdlog::error("Pipeline '{}': {} blackboard key '{}' not set",
                      name(), what, p.key);
        return -1;
    }
    if (auto* i64 = std::get_if<int64_t>(&*v)) {
        return static_cast<int>(*i64 < 0 ? 0 : *i64);
    }
    if (auto* d = std::get_if<double>(&*v)) {
        if (*d == std::floor(*d)) return static_cast<int>(*d < 0 ? 0 : *d);
    }
    if (auto* r = std::get_if<LuaRef>(&*v)) {
        lua_State* L = lua_state_;  // captured at Init (registry refs live there)
        if (L != nullptr && (*r)->ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, (*r)->ref);
            if (lua_istable(L, -1)) {
                std::optional<int> nums[2];
                for (int k = 1; k <= 2; ++k) {
                    lua_rawgeti(L, -1, k);
                    if (lua_isinteger(L, -1)) {
                        nums[k - 1] = static_cast<int>(lua_tointeger(L, -1));
                    } else if (lua_isnumber(L, -1)) {
                        double d = lua_tonumber(L, -1);
                        if (d == std::floor(d)) nums[k - 1] = static_cast<int>(d);
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);  // the table
                if (nums[0].has_value()) {
                    int lo = *nums[0], hi = nums[1].value_or(lo);
                    if (lo < 0) lo = 0;
                    if (hi < 0) hi = 0;
                    return lo <= hi ? RollIntInRange(lo, hi, rng_)
                                    : RollIntInRange(hi, lo, rng_);
                }
            } else {
                lua_pop(L, 1);
            }
        }
    }
    spdlog::error("Pipeline '{}': {} blackboard key '{}' must be an integer or a {{lo,hi}} table",
                  name(), what, p.key);
    return -1;
}

void Pipeline::CancelPendingResolves() {
    if (lua_ctx_ != nullptr) {
        if (resolve_timeout_.co != nullptr) lua_ctx_->CancelCall(resolve_timeout_.co);
        if (resolve_retry_.co != nullptr) lua_ctx_->CancelCall(resolve_retry_.co);
    }
    resolve_timeout_ = {};
    resolve_retry_ = {};
}

// Resolve this step's budgets for the CURRENT run when it enters its wait
// window. Fixed/range params (and the pipeline defaults) resolve inline; a
// `$key` ref reads the blackboard — a static value finishes inline, while
// a provider-served key launches the provider's get as a coroutine and the
// pipeline parks in kResolve until both reads complete. Returns false when
// a resolve is in flight.
bool Pipeline::ResolveStepBudgets(Blackboard& bb, size_t i) {
    bool in_flight = false;
    auto resolve = [&](const EdgeParam& e, const EdgeParam& def, const char* what,
                       int& cur, PendingValue& pend) {
        if (cur >= 0) return;  // already resolved this run
        const EdgeParam& p =
            (e.kind == EdgeParam::Kind::kFixed && e.lo < 0) ? def : e;
        if (p.kind == EdgeParam::Kind::kFixed) {
            cur = p.lo < 0 ? 0 : p.lo;
            return;
        }
        if (p.kind == EdgeParam::Kind::kRange) {
            cur = RollIntInRange(p.lo, p.hi, rng_);
            return;
        }
        // kBbRef: read the blackboard fresh for this run.
        auto look = bb.Lookup(p.key);
        switch (look.kind) {
        case BbReadResult::Kind::kValue:
            cur = FinishEdgeParam(std::move(look.value), p, what);
            break;
        case BbReadResult::Kind::kMissing:
            cur = FinishEdgeParam(std::nullopt, p, what);
            break;
        case BbReadResult::Kind::kProvider: {
            if (!lua_ctx_) {
                spdlog::error("Pipeline '{}': {} provider key '{}' has no LuaRuntime",
                              name(), what, p.key);
                cur = 0;
                break;
            }
            pend = {};
            pend.src = p;
            pend.what = what;
            auto out = provider_call::InvokeProviderGet(
                lua_ctx_, look.provider, look.path,
                [&pend](ScriptResult r) {
                    pend.done = true;
                    pend.value = (!r.values.empty())
                                     ? std::optional<LuaValue>(std::move(r.values[0]))
                                     : std::nullopt;
                });
            if (out.yielded) {
                pend.co = out.co;
                in_flight = true;
            } else {
                // Synchronous provider completion: the callback fired.
                int done = FinishEdgeParam(pend.value, pend.src, pend.what);
                pend = {};
                cur = done < 0 ? 0 : done;
            }
            break;
        }
        }
        if (cur < 0) cur = 0;  // failed ref = wait forever / no retry
    };
    resolve(steps_[i].timeout, def_timeout_, "*timeout", cur_timeout_[i],
            resolve_timeout_);
    resolve(steps_[i].retry, def_retry_, "*retry", cur_retry_[i],
            resolve_retry_);
    if (in_flight) {
        phase_ = Phase::kResolve;
        return false;
    }
    return true;
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
            CancelPendingResolves();  // stale budgets for the aborted step
            children_[current_child_index_]->OnAborted();
            current_child_index_ = i;
            children_[i]->Reset();
            retries_used_[i] = 0;
            cur_timeout_[i] = -1;
            cur_retry_[i] = -1;
            pending_action_error_.clear();  // older step's failure no longer pending
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
                // Budgets first (a provider-backed `$key` resolve parks in
                // kResolve for a few ticks); the wait starts once final.
                if (!ResolveStepBudgets(bb, current_child_index_)) {
                    return NodeStatus::kRunning;
                }
                last_action_failed_ = false;  // target met preempted the action
                phase_ = Phase::kWait;  // the wait below advances this tick
                wait_start_ms_ = events.now_ms();
                continue;  // fall into the wait with the target already met
            }

            NodeStatus s = children_[current_child_index_]->TickAndRecord(bb, events);
            if (s == NodeStatus::kRunning) return NodeStatus::kRunning;
            if (s == NodeStatus::kFailure) {
                // Two Failure sources — guard short-circuit (TickAndRecord
                // aborted the action because its abort=Self condition
                // flipped Failure) vs the action's own Tick returning
                // Failure. The node's last_tick_was_guard bit discriminates.
                if (children_[current_child_index_]->last_tick_was_guard()) {
                    // Guard-failure path: re-evaluate the guard every tick;
                    // on guard Success re-tick the action the same tick; on
                    // guard sustained Failure for *timeout ms → Pipeline
                    // failure. No *retry budget consumed (a guard loss is
                    // an environmental change, not a step-internal retry).
                    if (!ResolveStepBudgets(bb, current_child_index_)) {
                        return NodeStatus::kRunning;  // $key provider still resolving
                    }
                    phase_ = Phase::kWaitGuard;
                    wait_start_ms_ = events.now_ms();
                    return NodeStatus::kRunning;  // first guard-wait tick
                }
                // Action-failure path: post-failure wait sharing the
                // *timeout/*retry budget with target-not-met. The *target is
                // still evaluated every tick (a failed action's side effects
                // may have met it); for a bare step (no *target) a failed
                // action reads as not-complete, so it always re-runs or fails
                // OnWaitTimeout. last_action_failed_ is set BEFORE budget
                // resolve so it survives a kResolve park.
                if (!children_[current_child_index_]->last_error().empty()) {
                    pending_action_error_ =
                        children_[current_child_index_]->last_error();
                }
                last_action_failed_ = true;
                if (!ResolveStepBudgets(bb, current_child_index_)) {
                    return NodeStatus::kRunning;
                }
                phase_ = Phase::kWait;
                wait_start_ms_ = events.now_ms();
                return NodeStatus::kRunning;  // first post-failure tick
            }
            // Action done → wait for this step's target (absent target: met).
            // Budgets first (a provider-backed `$key` resolve parks in
            // kResolve for a few ticks); the wait window — and its clock —
            // starts once the budgets are final.
            if (!ResolveStepBudgets(bb, current_child_index_)) {
                return NodeStatus::kRunning;
            }
            // If the previous action run failed and we're here because the
            // retry succeeded, drop the stale error — it's no longer the
            // pending failure.
            pending_action_error_.clear();
            last_action_failed_ = false;  // this action run succeeded
            phase_ = Phase::kWait;
            wait_start_ms_ = events.now_ms();
            // fall through to the wait this tick
        }

        if (phase_ == Phase::kResolve) {
            // In-flight `$key` provider reads for the current step's
            // budgets: wait for BOTH sides, then enter the wait window.
            if (resolve_timeout_.co != nullptr && !resolve_timeout_.done) {
                return NodeStatus::kRunning;
            }
            if (resolve_retry_.co != nullptr && !resolve_retry_.done) {
                return NodeStatus::kRunning;
            }
            const size_t i = current_child_index_;
            if (resolve_timeout_.co != nullptr) {
                int t = FinishEdgeParam(resolve_timeout_.value, resolve_timeout_.src,
                                        resolve_timeout_.what);
                cur_timeout_[i] = t < 0 ? 0 : t;
                resolve_timeout_ = {};
            }
            if (resolve_retry_.co != nullptr) {
                int r = FinishEdgeParam(resolve_retry_.value, resolve_retry_.src,
                                        resolve_retry_.what);
                cur_retry_[i] = r < 0 ? 0 : r;
                resolve_retry_ = {};
            }
            phase_ = Phase::kWait;
            wait_start_ms_ = events.now_ms();
            // fall through to the wait this tick
        }

        // phase == kWait: entered after this step's action ran — whether it
        // returned Success or Failure (last_action_failed_). Eval the current
        // step's *target EVERY tick: a "failed" action may still have
        // produced the target as a side effect, so it's checked the same way
        // as a successful one. One difference matters for bare steps (no
        // *target): EvalTarget treats an absent target as met (action
        // completion IS the target), which is right after a Success but
        // wrong after a Failure — a failed action is emphatically not
        // complete, so it must re-run (via OnWaitTimeout) rather than advance.
        if (phase_ == Phase::kWait) {
            NodeStatus s;
            if (last_action_failed_ &&
                steps_[current_child_index_].target == nullptr) {
                s = NodeStatus::kFailure;  // failed bare action: not complete
            } else {
                s = EvalTarget(bb, events, current_child_index_);
            }
            if (s == NodeStatus::kSuccess) {
                // Target met (possibly by a failed run's side effects) ->
                // advance past this step and re-scan. The step's pending
                // failure no longer matters: the goal is achieved.
                pending_action_error_.clear();
                ++current_child_index_;
                if (current_child_index_ >= n) return NodeStatus::kSuccess;
                phase_ = Phase::kScan;
                continue;  // scan (and possibly act) this tick
            }
            // Running or Failure → target not yet met; check the wall-clock budget.
            int timeout = cur_timeout_[current_child_index_];
            if (timeout > 0 && events.now_ms() - wait_start_ms_ >= timeout) {
                return OnWaitTimeout(bb, events);
            }
            return NodeStatus::kRunning;
        }

        // phase == kWaitGuard: the node's guard (abort=Self condition)
        // short-circuited mid-action. Re-evaluate the guard every tick
        // via Node::GuardStatus (stale-while-running). Guard Success →
        // fall back to kAct and re-tick the action the same tick (NO
        // child Reset — the action's pre-abort state is preserved).
        // Guard held Failure for *timeout ms → Pipeline failure
        // (no OnWaitTimeout, no *retry consumed, no child Reset).
        if (phase_ == Phase::kWaitGuard) {
            NodeStatus g =
                children_[current_child_index_]->GuardStatus(bb, events);
            if (g == NodeStatus::kSuccess) {
                // Guard recovered: re-tick the action this tick. The
                // action's is_running_/last_tick_status_ were cleared by
                // the TickAndRecord short-circuit that brought us here, so
                // the next Tick runs fresh.
                phase_ = Phase::kAct;
                continue;  // fall into kAct in this Tick() loop
            }
            int timeout = cur_timeout_[current_child_index_];
            if (timeout > 0 && events.now_ms() - wait_start_ms_ >= timeout) {
                // Guard held Failure for the full window — fail the
                // pipeline. No action error to surface (the action itself
                // never returned Failure; the guard did).
                const size_t i = current_child_index_;
                std::string msg = "Pipeline '" + name() + "': step " +
                                  std::to_string(i) +
                                  " guard held Failure for " +
                                  std::to_string(timeout) + "ms";
                set_last_error(std::move(msg));
                return NodeStatus::kFailure;
            }
            return NodeStatus::kRunning;
        }
    }
}

NodeStatus Pipeline::OnWaitTimeout(Blackboard& bb, BtEventQueue& events) {
    const size_t i = current_child_index_;

    // React to a regressed PRECONDITION before retrying. Each step's *target
    // is the NEXT step's condition (the pipeline only reached this step
    // because it held). Its first run is safe — the condition was just
    // verified to advance — but before re-running the action on a retry we
    // must confirm the precondition still holds. Walk the prefix from the
    // nearest predecessor backwards and, if any step's target no longer holds,
    // redo that step (its own re-run + re-scan will re-establish the whole
    // chain) instead of blindly retrying against a gone precondition. This is
    // unconditional on the target's `abort` mode — `abort=LowerPriority` only
    // additionally enables the per-tick reactive version (EvaluateTargetAborts
    // resets `current` before this runs, so that path is never re-taken here).
    for (size_t p = i; p > 0; --p) {
        NodeCondition* t = steps_[p - 1].target.get();
        if (t == nullptr) continue;  // bare predecessor: no condition to re-establish
        if (EffectiveTarget(*t, bb, events) != NodeStatus::kSuccess) {
            size_t redo = p - 1;
            spdlog::info("Pipeline '{}': step {} precondition regressed "
                         "(met->unmet), redoing step {} before retrying step {}",
                         name(), redo, redo, i);
            // Abandon the current step's stale wait state and re-enter the
            // regressed predecessor: fresh act phase, fresh budgets. The redo
            // is not charged against the current step's *retry counter.
            CancelPendingResolves();
            children_[i]->Reset();
            children_[redo]->Reset();
            current_child_index_ = redo;
            retries_used_[redo] = 0;
            cur_timeout_[redo] = -1;
            cur_retry_[redo] = -1;
            pending_action_error_.clear();
            last_action_failed_ = false;
            phase_ = Phase::kAct;
            return NodeStatus::kRunning;
        }
    }

    // Retry budget exhausted (cur_retry_[i] is resolved: we entered this wait
    // window first) → the step's target never came, OR the action kept
    // returning failure across all retries.
    if (retries_used_[i] >= cur_retry_[i]) {
        std::string msg = "Pipeline '" + name() + "': step " +
                          std::to_string(i) +
                          (pending_action_error_.empty()
                               ? std::string(" target not met after action + retries")
                               : std::string(" failed after retries: ") + pending_action_error_);
        set_last_error(std::move(msg));
        pending_action_error_.clear();
        return NodeStatus::kFailure;
    }

    // Re-run this step's action, then wait for its target again (with a
    // fresh ms budget) on subsequent ticks. The next action run may fail
    // again — in which case kAct will re-enter kWait with last_action_failed_
    // set afresh (the retry counter is what actually limits attempts; the
    // error gets re-captured then).
    ++retries_used_[i];
    children_[i]->Reset();
    phase_ = Phase::kAct;
    return NodeStatus::kRunning;  // tick the re-run action on the next tick
}

void Pipeline::Reset() {
    started_ = false;
    phase_ = Phase::kScan;
    wait_start_ms_ = 0;
    CancelPendingResolves();
    std::fill(retries_used_.begin(), retries_used_.end(), 0);
    std::fill(cur_timeout_.begin(), cur_timeout_.end(), -1);  // re-roll next run
    std::fill(cur_retry_.begin(), cur_retry_.end(), -1);
    std::fill(target_prev_.begin(), target_prev_.end(), NodeStatus::kFailure);
    pending_action_error_.clear();
    last_action_failed_ = false;
    for (auto& step : steps_) {
        if (step.target) step.target->Reset();
    }
    Composite::Reset();  // current_child_index_ = 0; reset children
}

void Pipeline::OnAborted() {
    started_ = false;
    phase_ = Phase::kScan;
    wait_start_ms_ = 0;
    CancelPendingResolves();
    std::fill(cur_timeout_.begin(), cur_timeout_.end(), -1);
    std::fill(cur_retry_.begin(), cur_retry_.end(), -1);
    std::fill(target_prev_.begin(), target_prev_.end(), NodeStatus::kFailure);
    pending_action_error_.clear();
    last_action_failed_ = false;
    Composite::OnAborted();
}
