#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "blackboard.h"
#include "node.h"
#include "path_tracer.h"
#include "types.h"

class BtEventQueue;

class BehaviorTreeEngine : public std::enable_shared_from_this<BehaviorTreeEngine> {
public:
    using Ptr = std::shared_ptr<BehaviorTreeEngine>;

    explicit BehaviorTreeEngine(std::shared_ptr<Blackboard> bb = {});
    ~BehaviorTreeEngine();

    BehaviorTreeEngine(const BehaviorTreeEngine&) = delete;
    BehaviorTreeEngine& operator=(const BehaviorTreeEngine&) = delete;
    BehaviorTreeEngine(BehaviorTreeEngine&&) = delete;
    BehaviorTreeEngine& operator=(BehaviorTreeEngine&&) = delete;

    // Install an already-parsed tree root. Resets tree state.
    void SetRoot(std::unique_ptr<Node> root);

    // Reset tree state and release all resources
    void Stop();

    // Synchronous single tick
    NodeStatus TickOnce();

    bool IsLoaded() const { return root_ != nullptr; }
    std::string GetStatus() const;

    Blackboard& blackboard() { return *blackboard_; }
    const std::shared_ptr<Blackboard>& shared_blackboard() const { return blackboard_; }

    // Async-load all Script nodes (and subtrees thereof). Also inits every
    // node's guard `condition` via InitConditionsRecursive.
    async_simple::coro::Lazy<std::string> InitScriptNodesAsync(lua_State* L, LuaRuntime* ctx);

    // Generation counter — incremented on Stop(). Async init checks this to detect stale operations.
    uint64_t generation() const { return generation_; }

    // Path tracing — records per-tick active paths for post-mortem reports.
    PathTracer& path_tracer() { return tracer_; }
    const PathTracer& path_tracer() const { return tracer_; }
    void SetTracing(bool on) { tracer_.set_tracing(on); }

    // Live tick-path debug logging. When true, TickOnce prints the active
    // path(s) for every tick via spdlog, so a run can be followed as it
    // advances/switches without waiting for a post-mortem report. Also raises
    // the default logger to info+ and flushes per line, so the traces are
    // visible even if the host capped the level or buffers piped output.
    void SetDebug(bool on);
    bool debug() const { return debug_; }

    // --- Lifecycle state machine (driven by bt_library; engine holds data) ---
    enum class BtState { kIdle, kReady, kRunning, kPaused, kSuccess, kFailure };
    struct RunOutcome { bool done = false; std::string status; std::string error; };

    BtState state() const { return state_; }
    void set_state(BtState s) { state_ = s; }

    // Detail string for the last terminal failure — the offending node's
    // last_error() propagated up to the root (e.g. a script's
    // "file:line: message"). Captured before ResetTree() clears node state, so
    // it stays valid after TickOnce() returns. Empty unless the last run ended
    // in failure. Surfaced to Lua as bt.exec's second return value.
    const std::string& last_error() const { return last_error_; }

    const RunOutcome& last_outcome() const { return last_outcome_; }
    void SetOutcome(std::string status, std::string error) {
        last_outcome_.done = true;
        last_outcome_.status = std::move(status);
        last_outcome_.error = std::move(error);
    }
    void ClearOutcome() { last_outcome_ = RunOutcome{}; }

    bool has_await_handle() const { return await_handle_.has_value(); }
    int64_t await_handle() const { return await_handle_.value_or(0); }
    void set_await_handle(int64_t h) { await_handle_ = h; }
    void clear_await_handle() { await_handle_.reset(); }

    // Position the tree at a root→leaf path given by node names. Returns an
    // error string on failure, empty on success. Illegal while running;
    // rejects Parallel (no single active child) and Random* (shuffle order).
    std::optional<std::string> GotoPath(const std::vector<std::string>& names);

private:
    void ResetTree();
    // Log the active path(s) of the just-finished tick (debug_ only).
    void LogActivePaths(const std::vector<std::vector<Node*>>& paths,
                        NodeStatus root_status);
    // Reactive abort (LowerPriority / Both): walk the tree, evaluate guard
    // conditions, and on a false→true flip preempt the currently-running
    // lower-priority branch. Self abort is handled per-node in Node::TickAndRecord.
    void EvaluateAborts();
    void EvaluateAbortsRecursive(Node* node, BtEventQueue& events);
    // On a LowerPriority flip at `source`, walk up to the nearest ancestor
    // Composite where source's branch outranks the running branch; abort that
    // running branch and route the composite to source's branch.
    void PreemptLowerPriority(Node* source);
    // Recursive async init of every node's guard condition (if any). Walks
    // Composite / SingleChildNode / SubtreeNode children so every node is
    // visited regardless of how each Node::Init override chains.
    async_simple::coro::Lazy<bool> InitConditionsRecursive(Node* node, lua_State* L, LuaRuntime* ctx);
    // Ordered, possibly multi-chain active path(s). Parallel fans out to one
    // chain per child; other composites/single-child nodes follow their active
    // child; leaves yield a single [self] chain.
    void CollectActivePaths(Node* node, std::vector<std::vector<Node*>>& out);

    std::unique_ptr<Node> root_;
    bool debug_ = false;
    std::shared_ptr<Blackboard> blackboard_;
    std::unordered_map<Node*, NodeStatus> cond_state_;  // prev effective status (flip detection)

    std::string last_error_;
    NodeStatus last_status_ = NodeStatus::kRunning;
    uint64_t generation_ = 0;

    BtState state_ = BtState::kIdle;
    RunOutcome last_outcome_;
    std::optional<int64_t> await_handle_;  // AsyncHandle for bt.await()

    PathTracer tracer_;
};
