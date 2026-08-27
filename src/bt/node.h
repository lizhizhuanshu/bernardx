#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "node_condition.h"
#include "types.h"

class Blackboard;
class BtEventQueue;
class LuaRuntime;

class Node {
public:
    virtual ~Node() = default;

    virtual NodeStatus Tick(Blackboard& bb, BtEventQueue& events) = 0;

    // Non-virtual wrapper that records this node's own Tick() return into
    // last_tick_status_ for path tracing. Internal parent→child invocations
    // must use this (not Tick) so every node — including leaves — has its
    // last status populated. The recorded value reflects the node's TRUE
    // return: wrappers that rewrite status (Repeat/Retry) do so AFTER this
    // returns, leaving the child's recorded value untouched.
    //
    // Also enforces the node's guard `condition` (if any): each tick the guard
    // is evaluated with stale-while-running (an async condition mid-re-eval
    // uses its last terminal result). If the guard is (effectively) Failure,
    // the node is interrupted — OnAborted is called if it was mid-flight, and
    // Failure is returned without ticking. This is the general interruption
    // mechanism: it applies to every node that carries a condition.
    //
    // Returns a kFailure to the caller through TWO distinct mechanisms that
    // the caller (Pipeline) needs to disambiguate:
    //   - the guard short-circuited before the action ran (last_tick_was_guard
    //     = true, OnAborted invoked if the action was previously running)
    //   - the action's Tick() ran and itself returned kFailure
    //     (last_tick_was_guard = false)
    // last_tick_was_guard_ is a per-tick signal — the caller reads it
    // immediately after TickAndRecord returns and must not cache it.
    NodeStatus TickAndRecord(Blackboard& bb, BtEventQueue& events) {
        // Self abort: while this node is active, monitor its guard condition and
        // interrupt on Failure. Applies only when the condition's abort mode is
        // Self or Both (LowerPriority preemption is handled by the engine).
        if (condition_) {
            AbortMode am = condition_->abort();
            if (am == AbortMode::kSelf || am == AbortMode::kBoth) {
                NodeStatus raw = condition_->Eval(bb, events);
                NodeStatus guard = (raw == NodeStatus::kRunning)
                                       ? condition_->last_terminal()
                                       : raw;
                if (guard == NodeStatus::kFailure) {
                    if (is_running_) {
                        // Guard short-circuit: the action's in-flight work
                        // IS interrupted (its OnAborted runs, cancelling any
                        // yielded coroutine and calling script Abort/Exit
                        // hooks), but the condition's own state MUST be
                        // preserved — the Pipeline's kWaitGuard phase will
                        // re-evaluate this same condition every tick to
                        // detect recovery. Resetting the condition here would
                        // re-Enter the script (re-initializing its counter
                        // to 0) and the guard would spuriously "recover".
                        // self_aborted_by_guard_ tells that OnAborted to skip
                        // the condition Reset. It runs synchronously between
                        // the set here and the return, so it is the facet
                        // consumed by OnAborted; last_tick_was_guard_ (set
                        // below) is the sibling facet that survives to the
                        // caller. Two bits are deliberate — a single bit
                        // consumed by OnAborted would be gone by the return.
                        self_aborted_by_guard_ = true;
                        OnAborted();  // interrupt mid-flight work
                    }
                    is_running_ = false;
                    last_tick_status_ = NodeStatus::kFailure;
                    last_tick_was_guard_ = true;  // guard short-circuit, not action failure
                    return NodeStatus::kFailure;
                }
            }
        }
        last_tick_was_guard_ = false;  // action's Tick() runs; failure (if any) is action's own
        last_tick_status_ = Tick(bb, events);
        is_running_ = (last_tick_status_ == NodeStatus::kRunning);
        return last_tick_status_;
    }

    virtual void Reset();
    virtual void OnAborted();

    virtual async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx);

    // Tree structure
    Node* parent() const { return parent_; }
    void set_parent(Node* p) { parent_ = p; }

    uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::string& type() const { return type_; }
    const std::string& description() const { return description_; }
    void set_description(std::string desc) { description_ = std::move(desc); }

    const std::string& last_error() const { return last_error_; }
    void set_last_error(std::string err) { last_error_ = std::move(err); }

    NodeStatus last_tick_status() const { return last_tick_status_; }

    // True iff the most recent TickAndRecord returned kFailure via the guard
    // short-circuit (abort=Self condition flipped Failure) rather than via the
    // action's own Tick() returning kFailure. Per-tick signal — only meaningful
    // immediately after the matching TickAndRecord. Pipeline reads this to
    // decide between its action-failure (the post-failure kWait window) and
    // guard-failure (kWaitGuard) wait phases. (Its sibling facet,
    // self_aborted_by_guard_, is the one OnAborted consumes synchronously.)
    bool last_tick_was_guard() const { return last_tick_was_guard_; }

    // Optional guard condition. Composites gate children on it before ticking
    // (see composite.cc) and the engine monitors it reactively per its abort
    // mode — guarding is explicit, never implicit in `Tick`. (Pipeline's
    // `*target` is a separate mechanism: it lives on the pipeline's Step, not
    // on the node.)
    void SetCondition(std::shared_ptr<NodeCondition> c) { condition_ = std::move(c); }
    NodeCondition* condition() const { return condition_.get(); }
    // Shared-ownership view of the guard condition. Used by Subtree's
    // `@child_condition` marker to adopt the embedded subtree root's condition
    // (shared, not copied) so the boundary gate and the inside evaluate the
    // same condition object.
    std::shared_ptr<NodeCondition> shared_condition() const { return condition_; }

    // Effective guard status (Success if there is no condition). Evaluates the
    // condition with stale-while-running; composites call this to GATE a child
    // before ticking it (Failure → Selector skips / Sequence fails / Parallel
    // counts a failure). Does NOT tick this node.
    NodeStatus GuardStatus(Blackboard& bb, BtEventQueue& events) {
        if (!condition_) return NodeStatus::kSuccess;
        NodeStatus raw = condition_->Eval(bb, events);
        return (raw == NodeStatus::kRunning) ? condition_->last_terminal() : raw;
    }

protected:
    Node(uint32_t id, std::string type, std::string name);

    Node* parent_ = nullptr;
    uint32_t id_;
    std::string type_;
    std::string name_;
    std::string description_;
    std::string last_error_;
    NodeStatus last_tick_status_ = NodeStatus::kRunning;
    bool is_running_ = false;  // last Tick returned Running (interruptable)
    std::shared_ptr<NodeCondition> condition_;

private:
    // Two names for the SAME event — TickAndRecord's guard short-circuit (an
    // abort=Self condition faulted) — split into two bits because OnAborted()
    // runs BETWEEN the signal being raised and the return:
    //   self_aborted_by_guard_ — set on the guard branch just before OnAborted()
    //       runs. OnAborted consumes it one-shot: the condition->Reset() is
    //       skipped (the faulted guard must stay live for kWaitGuard to re-Eval
    //       it, wiping it would re-Enter the script and spuriously "recover").
    //       The one-shot also clears it, so a LATER unrelated OnAborted (an
    //       external preemption) resets the condition normally.
    bool self_aborted_by_guard_ = false;
    //   last_tick_was_guard_ — survives past OnAborted to the end of
    //       TickAndRecord; the caller reads it (last_tick_was_guard()) right
    //       after to tell a guard short-circuit from an action failure. Cleared
    //       by Reset and by the next action Tick. Two bits are required, not
    //       one: a single bit that OnAborted consumed would be gone before the
    //       caller could read it.
    bool last_tick_was_guard_ = false;
};
