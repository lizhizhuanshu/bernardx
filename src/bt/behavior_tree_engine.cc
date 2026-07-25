#include "behavior_tree_engine.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "bt_utils.h"
#include "composite.h"
#include "lua_runtime.h"
#include "parallel.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "single_child_node.h"
#include "subtree_node.h"
#include "time_utils.h"

BehaviorTreeEngine::BehaviorTreeEngine(std::shared_ptr<Blackboard> bb)
    : blackboard_(bb ? std::move(bb) : std::make_shared<Blackboard>()) {}

BehaviorTreeEngine::~BehaviorTreeEngine() {
    Stop();
}

void BehaviorTreeEngine::SetRoot(std::unique_ptr<Node> root) {
    DeactivateAllSensors();
    root_ = std::move(root);
    event_queue_.Drain();
    last_status_ = NodeStatus::kRunning;
    tracer_.SnapshotTopology(root_.get());
    tracer_.Reset();
}

void BehaviorTreeEngine::Stop() {
    DeactivateAllSensors();
    root_.reset();
    event_queue_.Drain();
    ClearDecoratorState();
    last_status_ = NodeStatus::kRunning;
    // NOTE: tracer data is intentionally preserved across Stop() so callers
    // can still dump_paths() after a timeout/abort. The next SetRoot() rebuilds.
    generation_++;
}

void BehaviorTreeEngine::Notify(const std::string& event_name, LuaValue data) {
    event_queue_.Push({event_name, std::move(data)});
}

std::string BehaviorTreeEngine::GetStatus() const {
    switch (state_) {
        case BtState::kIdle: return "idle";
        case BtState::kReady: return "ready";
        case BtState::kRunning: return "running";
        case BtState::kPaused: return "paused";
        case BtState::kSuccess: return "success";
        case BtState::kFailure: return "failure";
    }
    return "idle";
}

std::optional<std::string> BehaviorTreeEngine::GotoPath(
    const std::vector<std::string>& names) {
    if (state_ == BtState::kRunning) {
        return "goto_path illegal while running; pause first";
    }
    if (!root_) return "no tree loaded; call ready first";
    if (names.empty()) return "empty path";

    // Walk the live tree, recording (composite, child index) positions to set.
    struct Pos { Composite* comp; size_t idx; };
    std::vector<Pos> positions;
    Node* cursor = root_.get();
    if (cursor->name() != names[0]) {
        return "path[0] '" + names[0] + "' does not match root '" + cursor->name() + "'";
    }
    for (size_t i = 1; i < names.size(); ++i) {
        const std::string& target = names[i];
        if (dynamic_cast<Parallel*>(cursor)) {
            return "goto_path cannot traverse a Parallel node ('" + cursor->name() + "')";
        }
        if (dynamic_cast<RandomSelector*>(cursor) ||
            dynamic_cast<RandomSequence*>(cursor)) {
            return "goto_path unsupported through " + cursor->type();
        }
        if (auto* comp = dynamic_cast<Composite*>(cursor)) {
            size_t j = 0;
            for (; j < comp->children().size(); ++j) {
                if (comp->children()[j] && comp->children()[j]->name() == target) break;
            }
            if (j == comp->children().size()) {
                return "no child named '" + target + "' under '" + cursor->name() + "'";
            }
            positions.push_back({comp, j});
            cursor = comp->children()[j].get();
        } else if (auto* single = dynamic_cast<SingleChildNode*>(cursor)) {
            Node* child = single->child();
            if (!child || child->name() != target) {
                return "single-child node '" + cursor->name() + "' child is '" +
                       (child ? child->name() : std::string("none")) + "', not '" + target + "'";
            }
            cursor = child;
        } else {
            return "path descends into leaf '" + cursor->name() + "' before exhausting names";
        }
    }
    // Reset the whole tree (clears composite indices, leaf coroutine/counters,
    // Wait timers), then re-apply the recorded indices to position the path.
    root_->Reset();
    for (const auto& p : positions) {
        p.comp->set_current_child_index(p.idx);
    }
    ClearDecoratorState();
    return std::nullopt;
}

async_simple::coro::Lazy<std::string> BehaviorTreeEngine::InitScriptNodesAsync(lua_State* L, LuaRuntime* ctx) {
    if (root_) {
        if (!co_await root_->Init(L, ctx)) {
            co_return root_->last_error();
        }
    }
    co_return std::string();
}

NodeStatus BehaviorTreeEngine::TickOnce() {
    if (!root_) return NodeStatus::kFailure;

    tracer_.BeginTick();

    HandleEvents();
    TickSensors();

    EvaluateAllAbortMonitors();

    if (!EvaluateDecorators(root_.get())) {
        // Root decorator gate blocked the tick — tree is effectively stuck.
        // Still sample the (unchanged) active path so the report surfaces this.
        std::vector<std::vector<Node*>> paths;
        CollectActivePaths(root_.get(), paths);
        tracer_.OnTickDone(paths, NodeStatus::kRunning, NowMs(), "root 装饰器阻塞");
        last_status_ = NodeStatus::kRunning;
        return NodeStatus::kRunning;
    }

    auto status = root_->TickAndRecord(*blackboard_, event_queue_);

    {
        std::vector<std::vector<Node*>> paths;
        CollectActivePaths(root_.get(), paths);
        tracer_.OnTickDone(paths, status, NowMs(), nullptr);
    }

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

    last_status_ = status;
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
            // Record the flip for the path-trace timeline, but skip the very
            // first observation (no prior state) to avoid tick-0 spam.
            if (it != node_state.end()) {
                tracer_.OnDecoratorFlip(node, dec.get(), was, now);
            }
        }
        node_state[dec.get()] = now;  // always remember, so real flips are detected

        if (!now) {
            return false;
        }
    }
    return true;
}

void BehaviorTreeEngine::EvaluateAllAbortMonitors() {
    if (!root_) return;

    std::set<Node*> monitored;
    CollectActiveNodes(root_.get(), monitored);
    CollectAbortMonitoringNodes(root_.get(), monitored);
    monitored.erase(root_.get());

    for (auto* node : monitored) {
        EvaluateDecorators(node);
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
                        // Reset parent composite so it re-evaluates from the source child
                        composite->set_current_child_index(i);
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
        return;
    }
    auto* single = dynamic_cast<SingleChildNode*>(node);
    if (single && single->child()) {
        CollectRunningNodes(single->child(), out);
        return;
    }
    auto* sub = dynamic_cast<SubtreeNode*>(node);
    if (sub && sub->subtree_root()) {
        CollectRunningNodes(sub->subtree_root(), out);
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
        spdlog::debug("InitSensorsRecursive: sensor '{}', path='{}', interval={}",
                      spec.name, spec.script_path, spec.interval_ms);
        if (active_sensors_.count(spec.name)) {
            spdlog::warn("BehaviorTreeEngine: duplicate sensor name '{}', overwriting", spec.name);
        }
        auto sensor = std::make_unique<ActiveSensor>(spec);
        if (!co_await sensor->Init(L, ctx)) {
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
    } else if (auto* single = dynamic_cast<SingleChildNode*>(node)) {
        if (single->child()) {
            auto error = co_await InitSensorsRecursive(single->child(), L, ctx);
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

    for (auto* node : active_nodes) {
        if (!prev_sensor_nodes_.count(node)) {
            ActivateNodeSensors(node);
        }
    }

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
        return;
    }
    auto* single = dynamic_cast<SingleChildNode*>(node);
    if (single && single->child()) {
        CollectActiveNodes(single->child(), out);
        return;
    }
    auto* sub = dynamic_cast<SubtreeNode*>(node);
    if (sub && sub->subtree_root()) {
        CollectActiveNodes(sub->subtree_root(), out);
    }
}

void BehaviorTreeEngine::CollectActivePaths(Node* node,
                                            std::vector<std::vector<Node*>>& out) {
    if (!node) return;

    // Parallel: fan out to one chain per child (parallel semantics — every
    // child is concurrently active).
    if (auto* par = dynamic_cast<Parallel*>(node)) {
        std::vector<std::vector<Node*>> child_paths;
        for (const auto& child : par->children()) {
            CollectActivePaths(child.get(), child_paths);
        }
        if (child_paths.empty()) {
            out.push_back({node});
        } else {
            for (auto& cp : child_paths) {
                cp.insert(cp.begin(), node);
                out.push_back(std::move(cp));
            }
        }
        return;
    }

    // Sequential composite (Selector/Sequence/Random*): the current child only.
    if (auto* comp = dynamic_cast<Composite*>(node)) {
        Node* cur = (comp->current_child_index() < comp->children().size())
                        ? comp->children()[comp->current_child_index()].get()
                        : nullptr;
        if (cur) {
            std::vector<std::vector<Node*>> sub;
            CollectActivePaths(cur, sub);
            for (auto& s : sub) {
                s.insert(s.begin(), node);
                out.push_back(std::move(s));
            }
        } else {
            out.push_back({node});
        }
        return;
    }

    // SingleChild (Repeat/Retry/Subtree): its one child. SubtreeNode is a
    // SingleChildNode whose child() is the subtree root, so subtrees unfold.
    if (auto* single = dynamic_cast<SingleChildNode*>(node)) {
        Node* cur = single->child();
        if (cur) {
            std::vector<std::vector<Node*>> sub;
            CollectActivePaths(cur, sub);
            for (auto& s : sub) {
                s.insert(s.begin(), node);
                out.push_back(std::move(s));
            }
        } else {
            out.push_back({node});
        }
        return;
    }

    // Leaf.
    out.push_back({node});
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
        auto* single = dynamic_cast<SingleChildNode*>(node);
        if (single && single->child()) {
            CollectAbortMonitoringNodes(single->child(), out);
            return;
        }
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
