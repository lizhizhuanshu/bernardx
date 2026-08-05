#include "composite.h"

#include "bt_event_queue.h"
#include "bt_utils.h"
#include "blackboard.h"
#include "node.h"

Composite::Composite(uint32_t id, std::string type, std::string name)
    : Node(id, std::move(type), std::move(name)) {}

NodeStatus Composite::TickSequential(Blackboard& bb, BtEventQueue& events,
                                     SeqPolicy policy, ShuffledIndexTracker* tracker) {
    if (tracker) {
        tracker->EnsureShuffled(children_.size());
    }

    const size_t n = children_.size();
    for (size_t i = current_child_index_; i < n; ++i) {
        size_t idx = tracker ? tracker->order()[i] : i;
        auto& child = children_[idx];

        // Gate the child on its guard condition (UE4/5 decorator-style entry
        // check). A gated-Failure child is skipped by Selector and fails Sequence.
        if (child->GuardStatus(bb, events) == NodeStatus::kFailure) {
            if (policy == SeqPolicy::Sequence) {
                current_child_index_ = 0;
                return NodeStatus::kFailure;
            }
            continue;  // Selector: try the next child
        }

        switch (child->TickAndRecord(bb, events)) {
            case NodeStatus::kRunning:
                current_child_index_ = i;
                return NodeStatus::kRunning;
            case NodeStatus::kSuccess:
                if (policy == SeqPolicy::Selector) {
                    current_child_index_ = 0;
                    return NodeStatus::kSuccess;
                }
                break;  // Sequence: advance to next child
            case NodeStatus::kFailure:
                // Selector-family guards against clobbering an existing error
                // with an empty one (RandomSelector previously did not guard —
                // standardized here to match Selector/Parallel). Sequence-family
                // propagates unconditionally, then fails.
                if (policy == SeqPolicy::Selector) {
                    if (!child->last_error().empty()) {
                        set_last_error(child->last_error());
                    }
                    break;  // Selector: try next child
                }
                set_last_error(child->last_error());
                current_child_index_ = 0;
                return NodeStatus::kFailure;
        }
    }
    current_child_index_ = 0;
    return policy == SeqPolicy::Selector ? NodeStatus::kFailure : NodeStatus::kSuccess;
}

void Composite::AddChild(std::unique_ptr<Node> child) {
    child->set_parent(this);
    children_.push_back(std::move(child));
}

void Composite::Reset() {
    current_child_index_ = 0;
    for (auto& child : children_) {
        child->Reset();
    }
    Node::Reset();
}

void Composite::OnAborted() {
    current_child_index_ = 0;
    for (auto& child : children_) {
        child->OnAborted();
    }
    Node::OnAborted();
}

async_simple::coro::Lazy<bool> Composite::Init(lua_State* L, LuaRuntime* ctx) {
    for (auto& child : children_) {
        if (!co_await child->Init(L, ctx)) {
            set_last_error(child->last_error());
            co_return false;
        }
    }
    co_return true;
}
