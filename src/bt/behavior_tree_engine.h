#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"
#include "sensor.h"
#include "types.h"

class BehaviorTreeEngine : public std::enable_shared_from_this<BehaviorTreeEngine> {
public:
    using Ptr = std::shared_ptr<BehaviorTreeEngine>;

    explicit BehaviorTreeEngine(std::shared_ptr<Blackboard> bb = {});
    ~BehaviorTreeEngine();

    // Non-copyable
    BehaviorTreeEngine(const BehaviorTreeEngine&) = delete;
    BehaviorTreeEngine& operator=(const BehaviorTreeEngine&) = delete;

    // Load tree from JSON string
    std::pair<bool, std::string> Load(const std::string& json);

    // Lifecycle (thread-safe)
    void Run();
    void Pause();
    void Resume();
    void Stop();

    // Tick loop management — creates a dedicated LuaRuntime and runs TickOnce
    // in a loop on its executor until the tree completes or StopLoop is called.
    using CompletionCallback = std::function<void(const std::string& status, const std::string& error)>;

    void StartLoop(std::shared_ptr<CodeProvider> code_provider,
                   int64_t tick_interval_ms,
                   CompletionCallback on_complete,
                   LuaRuntime* parent_runtime);
    void StopLoop();
    bool IsLoopRunning() const { return loop_running_.load(); }

    // State queries (thread-safe)
    bool IsRunning() const { return running_.load(); }
    bool IsPaused() const { return paused_.load(); }
    std::string GetStatus() const;

    Blackboard& blackboard() { return *blackboard_; }

    // Event injection (thread-safe)
    void Notify(const std::string& event_name, LuaValue data);

    // Initialize script nodes with lua_State and LuaRuntime (called on event loop thread)
    // Async — each script's Init can yield (e.g., for async require)
    async_simple::coro::Lazy<std::string> InitScriptNodesAsync(lua_State* L, LuaRuntime* ctx);

    void SetProjectPath(std::string path) { project_path_ = std::move(path); }
    const std::string& project_path() const { return project_path_; }

    async_simple::coro::Lazy<std::string> InitSensorsAsync(lua_State* L, LuaRuntime* ctx);

    void ActivateInitialSensors();

    void DeactivateAllSensors();

    NodeStatus TickOnce();

private:
    using DecoratorState = std::unordered_map<Node*, std::unordered_map<Decorator*, bool>>;

    bool EvaluateDecorators(Node* node);
    void EvaluateDecoratorsRecursive(Node* node);
    void PropagateAbort(Node* source, AbortMode mode);
    void HandleEvents();
    void ResetTree();
    void ClearDecoratorState();
    void CollectRunningNodes(Node* node, std::vector<Node*>& out);
    bool IsDescendantOf(Node* node, Node* ancestor) const;

    // Sensor management
    void TickSensors();
    void UpdateActiveSensors();
    void CollectActiveNodes(Node* node, std::set<Node*>& out);
    void CollectAbortMonitoringNodes(Node* node, std::set<Node*>& out);
    void ActivateNodeSensors(Node* node);
    void DeactivateNodeSensors(Node* node, const std::set<Node*>& still_active);
    async_simple::coro::Lazy<std::string> InitSensorsRecursive(Node* node, lua_State* L, LuaRuntime* ctx);
    static bool HasAbortLowerPriority(const Node* node);

    std::unique_ptr<Node> root_;
    std::shared_ptr<Blackboard> blackboard_;
    BtEventQueue event_queue_;
    std::string project_path_;
    DecoratorState decorator_state_;

    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};

    std::map<std::string, std::unique_ptr<ActiveSensor>> active_sensors_;
    // Nodes that had sensors activated last tick
    std::set<Node*> prev_sensor_nodes_;

    // Tick loop state
    LuaRuntime::Ptr bt_context_;
    async_simple::coro::Lazy<void> TickLoop(LuaRuntime::Ptr ctx,
                                             int64_t tick_interval_ms,
                                             CompletionCallback on_complete);
    std::string last_error_;

    std::atomic<bool> loop_running_{false};
    std::mutex tick_loop_mu_;
    std::condition_variable tick_loop_cv_;
    bool tick_loop_exited_ = true;
};
