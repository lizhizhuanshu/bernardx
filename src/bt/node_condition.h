#pragma once

#include <string>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "types.h"

class Blackboard;
class BtEventQueue;
class LuaRuntime;

// A polled, first-class condition. Mirror of `Node`'s shape but for boolean
// guards: `Tick` returns the standard three states where, for a condition,
//   Success = met
//   Failure = not met
//   Running = still evaluating (an async script yielded; ask again next tick)
//
// Conditions are evaluated on demand by their owner (today: `Pipeline`'s
// first-tick scan) rather than scheduled/deferred. They are NOT part of the
// active-path / path-trace model, so they carry no id/parent/last_tick_status.
class NodeCondition {
public:
    virtual ~NodeCondition() = default;

    virtual NodeStatus Tick(Blackboard& bb, BtEventQueue& events) = 0;

    // Caching wrapper around Tick. On a terminal result (Success/Failure) it
    // remembers it in last_terminal_; returns the RAW status (incl. Running).
    // Callers that want stale-while-running semantics combine the two:
    //   effective = (raw == Running) ? last_terminal() : raw;
    // so an async condition mid-re-eval (Running) is treated as its last
    // terminal result instead of "unknown" — it won't spuriously interrupt a
    // running subordinate whose guard was previously met.
    NodeStatus Eval(Blackboard& bb, BtEventQueue& events) {
        NodeStatus s = Tick(bb, events);
        if (s != NodeStatus::kRunning) last_terminal_ = s;
        return s;
    }
    NodeStatus last_terminal() const { return last_terminal_; }

    // Reset evaluation state (cancel any yielded coroutine, tear down script).
    // Default is a no-op for stateless conditions.
    virtual void Reset() {}

    // Async setup (load script, resolve params). Default succeeds trivially.
    virtual async_simple::coro::Lazy<bool> Init(lua_State* /*L*/, LuaRuntime* /*ctx*/) {
        co_return true;
    }

    const std::string& type() const { return type_; }
    const std::string& last_error() const { return last_error_; }
    void set_last_error(std::string e) { last_error_ = std::move(e); }

    AbortMode abort() const { return abort_; }
    void set_abort(AbortMode m) { abort_ = m; }

protected:
    explicit NodeCondition(std::string type) : type_(std::move(type)) {}

    std::string type_;
    std::string last_error_;
    NodeStatus last_terminal_ = NodeStatus::kFailure;  // last non-Running result
    AbortMode abort_ = AbortMode::kNone;               // reactive abort policy
};
