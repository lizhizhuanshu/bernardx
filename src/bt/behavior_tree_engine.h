#pragma once

#include <functional>
#include <map>
#include <memory>
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

    BehaviorTreeEngine(const BehaviorTreeEngine&) = delete;
    BehaviorTreeEngine& operator=(const BehaviorTreeEngine&) = delete;
    BehaviorTreeEngine(BehaviorTreeEngine&&) = delete;
    BehaviorTreeEngine& operator=(BehaviorTreeEngine&&) = delete;

    // Load tree from JSON string
    std::pair<bool, std::string> Load(const std::string& json);

    // Reset tree state and release all resources
    void Stop();

    // Synchronous single tick
    NodeStatus TickOnce();

    bool IsLoaded() const { return root_ != nullptr; }
    std::string GetStatus() const;

    Blackboard& blackboard() { return *blackboard_; }

    void Notify(const std::string& event_name, LuaValue data);

    async_simple::coro::Lazy<std::string> InitScriptNodesAsync(lua_State* L, LuaRuntime* ctx);

    void SetProjectPath(std::string path) { project_path_ = std::move(path); }
    const std::string& project_path() const { return project_path_; }

    async_simple::coro::Lazy<std::string> InitSensorsAsync(lua_State* L, LuaRuntime* ctx);
    void ActivateInitialSensors();
    void DeactivateAllSensors();

    // Generation counter — incremented on Stop(). Async init checks this to detect stale operations.
    uint64_t generation() const { return generation_; }

private:
    using DecoratorState = std::unordered_map<Node*, std::unordered_map<Decorator*, bool>>;

    bool EvaluateDecorators(Node* node);
    void EvaluateAllAbortMonitors();
    void PropagateAbort(Node* source, AbortMode mode);
    void HandleEvents();
    void ResetTree();
    void ClearDecoratorState();
    void CollectRunningNodes(Node* node, std::vector<Node*>& out);
    bool IsDescendantOf(Node* node, Node* ancestor) const;

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

    std::map<std::string, std::unique_ptr<ActiveSensor>> active_sensors_;
    std::set<Node*> prev_sensor_nodes_;

    std::string last_error_;
    NodeStatus last_status_ = NodeStatus::kRunning;
    uint64_t generation_ = 0;
};
