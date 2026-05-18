#include "behavior_tree_engine.h"

#include <algorithm>
#include <chrono>

#include <spdlog/spdlog.h>

#include "composite.h"
#include "script_node.h"
#include "tree_parser.h"

int64_t BehaviorTreeEngine::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

BehaviorTreeEngine::BehaviorTreeEngine() = default;

BehaviorTreeEngine::~BehaviorTreeEngine() {
    Stop();
}

bool BehaviorTreeEngine::Load(const std::string& json) {
    DeactivateAllSensors();
    auto tree = TreeParser::Parse(json);
    if (!tree) {
        spdlog::error("BehaviorTreeEngine: failed to parse JSON");
        return false;
    }
    root_ = std::move(tree);
    blackboard_.Clear();
    event_queue_.Drain();
    return true;
}

void BehaviorTreeEngine::Run() {
    if (!root_) {
        spdlog::error("BehaviorTreeEngine: no tree loaded");
        return;
    }
    if (running_.load()) {
        spdlog::warn("BehaviorTreeEngine: already running");
        return;
    }

    running_.store(true);
    paused_.store(false);

    spdlog::info("BehaviorTreeEngine: started");
}

void BehaviorTreeEngine::Pause() {
    if (!running_.load() || paused_.load()) return;
    paused_.store(true);
}

void BehaviorTreeEngine::Resume() {
    if (!paused_.load()) return;
    paused_.store(false);
}

void BehaviorTreeEngine::Stop() {
    if (!running_.load()) return;

    running_.store(false);
    paused_.store(false);
    ResetTree();
    spdlog::info("BehaviorTreeEngine: stopped");
}

void BehaviorTreeEngine::Notify(const std::string& event_name, LuaValue data) {
    event_queue_.Push({event_name, std::move(data)});
}

std::string BehaviorTreeEngine::GetStatus() const {
    if (!running_.load()) return "stopped";
    if (paused_.load()) return "paused";
    return "running";
}

std::string BehaviorTreeEngine::GetCurrentNode() const {
    std::lock_guard<std::mutex> lock(current_node_mutex_);
    return current_node_path_;
}

async_simple::coro::Lazy<void> BehaviorTreeEngine::InitScriptNodesAsync(lua_State* L, LuaRuntime* ctx) {
    if (root_) {
        co_await InitScriptNodesRecursiveAsync(root_.get(), L, ctx);
    }
}

async_simple::coro::Lazy<void> BehaviorTreeEngine::InitScriptNodesRecursiveAsync(
    Node* node, lua_State* L, LuaRuntime* ctx) {
    if (auto* script = dynamic_cast<ScriptNode*>(node)) {
        co_await script->Init(L, ctx, project_path_);
    }
    if (auto* composite = dynamic_cast<Composite*>(node)) {
        for (auto& child : composite->children()) {
            co_await InitScriptNodesRecursiveAsync(child.get(), L, ctx);
        }
    }
}

NodeStatus BehaviorTreeEngine::TickOnce() {
    // Returns kRunning when paused/no-root so the BT event loop doesn't break.
    // Only success/failure cause the event loop to stop and resume the bt.run() coroutine.
    if (!root_ || !running_.load() || paused_.load()) return NodeStatus::kRunning;

    HandleEvents();
    TickSensors();

    if (!EvaluateDecorators(root_.get())) {
        return NodeStatus::kRunning;
    }

    auto status = root_->Tick(blackboard_, event_queue_);
    UpdateActiveSensors();

    if (status != NodeStatus::kRunning) {
        DeactivateAllSensors();
        ResetTree();
    }
    return status;
}

bool BehaviorTreeEngine::EvaluateDecorators(Node* node) {
    for (auto& dec : node->decorators()) {
        bool now = dec->Evaluate(blackboard_);
        bool was = false;
        auto it = node->prev_decorator_results_.find(dec.get());
        if (it != node->prev_decorator_results_.end()) {
            was = it->second;
        }

        if (now != was) {
            if (!now) {
                PropagateAbort(node, dec->abort_mode());
            } else {
                auto mode = dec->abort_mode();
                if (mode == AbortMode::kLowerPriority || mode == AbortMode::kBoth) {
                    PropagateAbort(node, AbortMode::kLowerPriority);
                }
            }
            node->prev_decorator_results_[dec.get()] = now;
        }

        if (!now) {
            return false;
        }
    }
    return true;
}

void BehaviorTreeEngine::PropagateAbort(Node* source, AbortMode mode) {
    if (mode == AbortMode::kNone) return;

    std::vector<Node*> running_nodes;
    CollectRunningNodes(root_.get(), running_nodes);

    std::vector<Node*> to_abort;

    if (mode == AbortMode::kSelf || mode == AbortMode::kBoth) {
        for (auto* node : running_nodes) {
            if (IsDescendantOf(node, source) || node == source) {
                to_abort.push_back(node);
            }
        }
    }

    if (mode == AbortMode::kLowerPriority || mode == AbortMode::kBoth) {
        Node* current = source;
        Node* parent = current->parent();
        while (parent) {
            auto* composite = dynamic_cast<Composite*>(parent);
            if (composite) {
                for (size_t i = 0; i < composite->children().size(); ++i) {
                    auto* child = composite->children()[i].get();
                    if (child == current || IsDescendantOf(current, child)) {
                        for (size_t j = i + 1; j < composite->children().size(); ++j) {
                            for (auto* node : running_nodes) {
                                if (IsDescendantOf(node, composite->children()[j].get())) {
                                    to_abort.push_back(node);
                                }
                            }
                        }
                        break;
                    }
                }
            }
            current = parent;
            parent = current->parent();
        }
    }

    std::sort(to_abort.begin(), to_abort.end());
    to_abort.erase(std::unique(to_abort.begin(), to_abort.end()), to_abort.end());

    for (auto* node : to_abort) {
        node->OnAborted();
    }
}

void BehaviorTreeEngine::HandleEvents() {
    event_queue_.Drain();
}

void BehaviorTreeEngine::ResetTree() {
    if (root_) {
        root_->Reset();
    }
}

void BehaviorTreeEngine::CollectRunningNodes(Node* node, std::vector<Node*>& out) {
    auto* composite = dynamic_cast<Composite*>(node);
    if (composite) {
        if (composite->is_mid_sequence()) {
            out.push_back(node);
        }
        for (auto& child : composite->children()) {
            CollectRunningNodes(child.get(), out);
        }
    }
}

bool BehaviorTreeEngine::IsDescendantOf(Node* node, Node* ancestor) const {
    while (node) {
        if (node == ancestor) return true;
        node = node->parent();
    }
    return false;
}

// --- Sensor management ---

void BehaviorTreeEngine::InitSensors(lua_State* L, LuaRuntime* ctx) {
    if (!root_) return;
    InitSensorsRecursive(root_.get(), L, ctx);
}

void BehaviorTreeEngine::InitSensorsRecursive(Node* node, lua_State* L, LuaRuntime* ctx) {
    for (auto& spec : node->sensor_specs()) {
        if (active_sensors_.count(spec.name)) {
            spdlog::warn("BehaviorTreeEngine: duplicate sensor name '{}', overwriting", spec.name);
        }
        auto sensor = std::make_unique<ActiveSensor>(spec);
        sensor->Init(L, ctx, project_path_);
        active_sensors_[spec.name] = std::move(sensor);
    }
    if (auto* composite = dynamic_cast<Composite*>(node)) {
        for (auto& child : composite->children()) {
            InitSensorsRecursive(child.get(), L, ctx);
        }
    }
}

void BehaviorTreeEngine::ActivateInitialSensors() {
    if (!root_) return;
    std::set<Node*> active_nodes;
    CollectActiveNodes(root_.get(), active_nodes);
    for (auto* node : active_nodes) {
        ActivateNodeSensors(node);
    }
    prev_sensor_nodes_ = std::move(active_nodes);
}

void BehaviorTreeEngine::TickSensors() {
    int64_t now = NowMs();
    for (auto& [name, sensor] : active_sensors_) {
        if (sensor->TickReady(now)) {
            sensor->RunOnce(blackboard_);
            sensor->ScheduleNext(now);
        }
    }
}

void BehaviorTreeEngine::UpdateActiveSensors() {
    if (!root_) return;

    std::set<Node*> active_nodes;
    CollectActiveNodes(root_.get(), active_nodes);

    // Activate sensors for newly active nodes
    for (auto* node : active_nodes) {
        if (!prev_sensor_nodes_.count(node)) {
            ActivateNodeSensors(node);
        }
    }

    // Deactivate sensors for no-longer-active nodes
    // (only if no other active node still needs the same sensor)
    for (auto* node : prev_sensor_nodes_) {
        if (!active_nodes.count(node)) {
            DeactivateNodeSensors(node, active_nodes);
        }
    }

    prev_sensor_nodes_ = std::move(active_nodes);
}

void BehaviorTreeEngine::CollectActiveNodes(Node* node, std::set<Node*>& out) {
    out.insert(node);
    auto* composite = dynamic_cast<Composite*>(node);
    if (composite && composite->current_child_index() < composite->children().size()) {
        CollectActiveNodes(composite->children()[composite->current_child_index()].get(), out);
    }
}

void BehaviorTreeEngine::ActivateNodeSensors(Node* node) {
    for (auto& spec : node->sensor_specs()) {
        auto it = active_sensors_.find(spec.name);
        if (it != active_sensors_.end() && !it->second->is_active()) {
            it->second->Activate(blackboard_);
        }
    }
}

void BehaviorTreeEngine::DeactivateNodeSensors(Node* node, const std::set<Node*>& still_active) {
    for (auto& spec : node->sensor_specs()) {
        // Check if any still-active node also declares this sensor
        bool still_needed = false;
        for (auto* other : still_active) {
            if (other == node) continue;
            for (auto& other_spec : other->sensor_specs()) {
                if (other_spec.name == spec.name) {
                    still_needed = true;
                    break;
                }
            }
            if (still_needed) break;
        }

        if (still_needed) continue;

        auto it = active_sensors_.find(spec.name);
        if (it != active_sensors_.end() && it->second->is_active()) {
            it->second->Deactivate(&blackboard_);
        }
    }
}

void BehaviorTreeEngine::DeactivateAllSensors() {
    for (auto& [name, sensor] : active_sensors_) {
        sensor->Deactivate(&blackboard_);
    }
    prev_sensor_nodes_.clear();
}
