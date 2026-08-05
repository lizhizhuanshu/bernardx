#include "behavior_tree_engine.h"

#include <spdlog/spdlog.h>

#include "composite.h"
#include "lua_runtime.h"
#include "node_condition.h"
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
    root_ = std::move(root);
    event_queue_.Drain();
    cond_state_.clear();
    last_status_ = NodeStatus::kRunning;
    tracer_.SnapshotTopology(root_.get());
    tracer_.Reset();
}

void BehaviorTreeEngine::Stop() {
    root_.reset();
    event_queue_.Drain();
    cond_state_.clear();
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
    if (!root_) return "no tree loaded; call init first";
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
    // Wait timers, and Pipeline scan state), then re-apply the recorded indices.
    root_->Reset();
    for (const auto& p : positions) {
        p.comp->set_current_child_index(p.idx);
    }
    return std::nullopt;
}

async_simple::coro::Lazy<std::string> BehaviorTreeEngine::InitScriptNodesAsync(lua_State* L, LuaRuntime* ctx) {
    if (root_) {
        if (!co_await root_->Init(L, ctx)) {
            co_return root_->last_error();
        }
        if (!co_await InitConditionsRecursive(root_.get(), L, ctx)) {
            co_return std::string("failed to init condition");
        }
    }
    co_return std::string();
}

async_simple::coro::Lazy<bool> BehaviorTreeEngine::InitConditionsRecursive(
    Node* node, lua_State* L, LuaRuntime* ctx) {
    if (!node) co_return true;
    if (auto* cond = node->condition()) {
        if (!co_await cond->Init(L, ctx)) {
            spdlog::error("BehaviorTreeEngine: condition init failed: {}", cond->last_error());
            co_return false;
        }
    }
    if (auto* comp = dynamic_cast<Composite*>(node)) {
        for (auto& child : comp->children()) {
            if (!co_await InitConditionsRecursive(child.get(), L, ctx)) co_return false;
        }
    } else if (auto* single = dynamic_cast<SingleChildNode*>(node)) {
        if (single->child()) {
            if (!co_await InitConditionsRecursive(single->child(), L, ctx)) co_return false;
        }
    }
    co_return true;
}

NodeStatus BehaviorTreeEngine::TickOnce() {
    if (!root_) return NodeStatus::kFailure;

    tracer_.BeginTick();

    HandleEvents();
    EvaluateAborts();  // LowerPriority/Both preemption before ticking

    auto status = root_->TickAndRecord(*blackboard_, event_queue_);

    {
        std::vector<std::vector<Node*>> paths;
        CollectActivePaths(root_.get(), paths);
        tracer_.OnTickDone(paths, status, NowMs(), nullptr);
    }

    if (status != NodeStatus::kRunning) {
        if (status == NodeStatus::kFailure) {
            last_error_ = root_->last_error();
        }
        ResetTree();
    }

    last_status_ = status;
    return status;
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
    cond_state_.clear();
}

void BehaviorTreeEngine::EvaluateAborts() {
    if (root_) EvaluateAbortsRecursive(root_.get());
}

void BehaviorTreeEngine::EvaluateAbortsRecursive(Node* node) {
    if (!node) return;
    if (NodeCondition* c = node->condition()) {
        AbortMode am = c->abort();
        if (am == AbortMode::kLowerPriority || am == AbortMode::kBoth) {
            NodeStatus raw = c->Eval(*blackboard_, event_queue_);
            NodeStatus eff = (raw == NodeStatus::kRunning) ? c->last_terminal() : raw;
            auto it = cond_state_.find(node);
            bool has_prev = (it != cond_state_.end());
            NodeStatus prev = has_prev ? it->second : NodeStatus::kFailure;
            cond_state_[node] = eff;
            // A false→true flip (not the first observation) preempts lower-priority
            // siblings so this branch can take over.
            if (has_prev && prev != NodeStatus::kSuccess && eff == NodeStatus::kSuccess) {
                PreemptLowerPriority(node);
            }
        }
    }
    if (auto* comp = dynamic_cast<Composite*>(node)) {
        for (auto& child : comp->children()) EvaluateAbortsRecursive(child.get());
    } else if (auto* single = dynamic_cast<SingleChildNode*>(node)) {
        if (single->child()) EvaluateAbortsRecursive(single->child());
    }
}

void BehaviorTreeEngine::PreemptLowerPriority(Node* source) {
    // Walk up through SingleChildNodes/Composites to the nearest ancestor
    // Composite where source's branch outranks (lower index than) the currently
    // running branch; abort that running branch and route the composite to it.
    Node* cur = source;
    while (cur) {
        Node* parent = cur->parent();
        auto* comp = dynamic_cast<Composite*>(parent);
        if (comp) {
            size_t i = 0;
            for (; i < comp->children().size(); ++i) {
                if (comp->children()[i].get() == cur) break;
            }
            size_t running = comp->current_child_index();
            if (i < comp->children().size() && running > i && running < comp->children().size()) {
                // `running` is a lower-priority branch (higher index) currently
                // active — abort it and route to source's branch.
                comp->children()[running]->OnAborted();
                comp->set_current_child_index(i);
                return;  // one preemption per flip
            }
        }
        cur = parent;
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

    // Sequential composite (Selector/Sequence/Pipeline/Random*): the current
    // child only.
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
