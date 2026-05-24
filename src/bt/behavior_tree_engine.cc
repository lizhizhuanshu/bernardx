#include "behavior_tree_engine.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "bt_utils.h"
#include "composite.h"
#include "lua_runtime.h"
#include "subtree_node.h"
#include "tree_parser.h"

BehaviorTreeEngine::BehaviorTreeEngine(std::shared_ptr<Blackboard> bb)
    : blackboard_(bb ? std::move(bb) : std::make_shared<Blackboard>()) {}

BehaviorTreeEngine::~BehaviorTreeEngine() {
    Stop();
}

std::pair<bool, std::string> BehaviorTreeEngine::Load(const std::string& json) {
    DeactivateAllSensors();
    auto result = TreeParser::Parse(json);
    if (!result.root) {
        auto err = result.error.empty() ? "failed to parse JSON" : result.error;
        spdlog::error("BehaviorTreeEngine: {}", err);
        return {false, std::move(err)};
    }
    root_ = std::move(result.root);
    blackboard_->Clear();
    event_queue_.Drain();
    return {true, {}};
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

async_simple::coro::Lazy<std::string> BehaviorTreeEngine::InitScriptNodesAsync(lua_State* L, LuaRuntime* ctx) {
    if (root_) {
        if (!co_await root_->Init(L, ctx, project_path_)) {
            co_return root_->last_error();
        }
    }
    co_return std::string();
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

    auto status = root_->Tick(*blackboard_, event_queue_);

    if (status == NodeStatus::kRunning) {
        UpdateActiveSensors();
    }

    if (status != NodeStatus::kRunning) {
        DeactivateAllSensors();
        if (status == NodeStatus::kFailure) {
            last_error_ = root_->last_error();
        }
        ResetTree();
    }
    return status;
}

bool BehaviorTreeEngine::EvaluateDecorators(Node* node) {
    auto& node_state = decorator_state_[node];
    for (auto& dec : node->decorators()) {
        bool now = dec->Evaluate(*blackboard_);
        bool was = false;
        auto it = node_state.find(dec.get());
        if (it != node_state.end()) {
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
            node_state[dec.get()] = now;
        }

        if (!now) {
            return false;
        }
    }
    return true;
}

void BehaviorTreeEngine::EvaluateDecoratorsRecursive(Node* node) {
    EvaluateDecorators(node);
    auto* composite = dynamic_cast<Composite*>(node);
    if (composite) {
        for (auto& child : composite->children()) {
            EvaluateDecoratorsRecursive(child.get());
        }
    }
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
    auto events = event_queue_.Drain();
    for (auto& evt : events) {
        blackboard_->Set("_event_" + evt.name, std::move(evt.data));
    }
}

void BehaviorTreeEngine::ResetTree() {
    if (root_) {
        root_->Reset();
    }
    ClearDecoratorState();
}

void BehaviorTreeEngine::ClearDecoratorState() {
    decorator_state_.clear();
}

void BehaviorTreeEngine::CollectRunningNodes(Node* node, std::vector<Node*>& out) {
    auto* composite = dynamic_cast<Composite*>(node);
    if (composite) {
        if (composite->has_started()) {
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

async_simple::coro::Lazy<std::string> BehaviorTreeEngine::InitSensorsAsync(lua_State* L, LuaRuntime* ctx) {
    if (!root_) co_return std::string();
    auto error = co_await InitSensorsRecursive(root_.get(), L, ctx);
    co_return error;
}

async_simple::coro::Lazy<std::string> BehaviorTreeEngine::InitSensorsRecursive(Node* node, lua_State* L, LuaRuntime* ctx) {
    for (auto& spec : node->sensor_specs()) {
        if (active_sensors_.count(spec.name)) {
            spdlog::warn("BehaviorTreeEngine: duplicate sensor name '{}', overwriting", spec.name);
        }
        auto sensor = std::make_unique<ActiveSensor>(spec);
        if (!co_await sensor->Init(L, ctx, project_path_)) {
            co_return "failed to init sensor '" + spec.name + "'";
        }
        active_sensors_[spec.name] = std::move(sensor);
    }
    if (auto* composite = dynamic_cast<Composite*>(node)) {
        for (auto& child : composite->children()) {
            auto error = co_await InitSensorsRecursive(child.get(), L, ctx);
            if (!error.empty()) co_return error;
        }
    } else if (auto* sub = dynamic_cast<SubtreeNode*>(node)) {
        if (sub->subtree_root()) {
            auto error = co_await InitSensorsRecursive(sub->subtree_root(), L, ctx);
            if (!error.empty()) co_return error;
        }
    }
    co_return std::string();
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
            sensor->RunOnce(*blackboard_);
            sensor->ScheduleNext(now);
        }
    }
}

void BehaviorTreeEngine::UpdateActiveSensors() {
    if (!root_) return;

    std::set<Node*> active_nodes;
    CollectActiveNodes(root_.get(), active_nodes);
    CollectAbortMonitoringNodes(root_.get(), active_nodes);

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

bool BehaviorTreeEngine::HasAbortLowerPriority(const Node* node) {
    for (const auto& dec : node->decorators()) {
        auto mode = dec->abort_mode();
        if (mode == AbortMode::kLowerPriority || mode == AbortMode::kBoth) {
            return true;
        }
    }
    return false;
}

void BehaviorTreeEngine::CollectAbortMonitoringNodes(Node* node, std::set<Node*>& out) {
    auto* composite = dynamic_cast<Composite*>(node);
    if (!composite) {
        auto* sub = dynamic_cast<SubtreeNode*>(node);
        if (sub && sub->subtree_root()) {
            CollectAbortMonitoringNodes(sub->subtree_root(), out);
        }
        return;
    }
    if (!out.count(node)) return;

    for (size_t i = 0; i < composite->current_child_index() && i < composite->children().size(); ++i) {
        auto* child = composite->children()[i].get();
        if (HasAbortLowerPriority(child)) {
            out.insert(child);
        }
    }

    if (composite->current_child_index() < composite->children().size()) {
        CollectAbortMonitoringNodes(composite->children()[composite->current_child_index()].get(), out);
    }
}

void BehaviorTreeEngine::ActivateNodeSensors(Node* node) {
    for (auto& spec : node->sensor_specs()) {
        auto it = active_sensors_.find(spec.name);
        if (it != active_sensors_.end() && !it->second->is_active()) {
            it->second->Activate(*blackboard_);
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
            it->second->Deactivate(blackboard_.get());
        }
    }
}

void BehaviorTreeEngine::DeactivateAllSensors() {
    for (auto& [name, sensor] : active_sensors_) {
        sensor->Deactivate(blackboard_.get());
    }
    prev_sensor_nodes_.clear();
}

// --- Tick loop management ---

void BehaviorTreeEngine::StartLoop(std::shared_ptr<CodeProvider> code_provider,
                                    int64_t tick_interval_ms,
                                    CompletionCallback on_complete,
                                    LuaRuntime* parent_runtime) {
    auto builder = LuaRuntime::Builder()
        .WithCodeProvider(std::move(code_provider));

    if (parent_runtime) {
        builder.InheritFrom(parent_runtime);
    }

    bt_context_ = builder.Create();

    {
        std::lock_guard lock(tick_loop_mu_);
        tick_loop_exited_ = false;
    }
    loop_running_.store(true);

    auto self = shared_from_this();
    auto ctx = bt_context_;
    bt_context_->executor()->schedule(
        [self, ctx, tick_interval_ms, cb = std::move(on_complete)]() mutable {
            if (!self->bt_context_) return;
            self->TickLoop(std::move(ctx), tick_interval_ms, std::move(cb))
                .via(self->bt_context_->executor())
                .start([self](auto&&) {});
        });
}

void BehaviorTreeEngine::StopLoop() {
    if (!loop_running_.load()) return;

    loop_running_.store(false, std::memory_order_release);
    DeactivateAllSensors();

    {
        std::unique_lock lock(tick_loop_mu_);
        tick_loop_cv_.wait(lock, [this] { return tick_loop_exited_; });
    }

    bt_context_.reset();
}

async_simple::coro::Lazy<void> BehaviorTreeEngine::TickLoop(
    LuaRuntime::Ptr ctx, int64_t tick_interval_ms,
    CompletionCallback on_complete) {
    auto done_guard = [this]() {
        {
            std::lock_guard lock(tick_loop_mu_);
            tick_loop_exited_ = true;
        }
        tick_loop_cv_.notify_all();
    };

    auto init_error = co_await InitScriptNodesAsync(ctx->main_state(), ctx.get());
    if (!init_error.empty()) {
        if (on_complete) {
            on_complete("failure", init_error);
        }
        done_guard();
        co_return;
    }

    auto sensor_error = co_await InitSensorsAsync(ctx->main_state(), ctx.get());
    if (!sensor_error.empty()) {
        if (on_complete) {
            on_complete("failure", sensor_error);
        }
        done_guard();
        co_return;
    }

    ActivateInitialSensors();
    Run();

    while (loop_running_.load(std::memory_order_acquire)) {
        auto status = TickOnce();

        if (status == NodeStatus::kSuccess || status == NodeStatus::kFailure) {
            std::string s = (status == NodeStatus::kSuccess) ? "success" : "failure";
            std::string err;
            if (status == NodeStatus::kFailure && !last_error_.empty()) {
                err = last_error_;
            }
            Stop();
            if (on_complete) {
                on_complete(s, err);
            }
            done_guard();
            co_return;
        }

        async_simple::Promise<void> p;
        auto f = p.getFuture();
        auto* ex = ctx->executor();
        ex->schedule([p = std::move(p)]() mutable { p.setValue(); },
                     std::chrono::milliseconds(tick_interval_ms));
        co_await std::move(f);
    }

    Stop();
    done_guard();
}
