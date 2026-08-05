#include "pipeline.h"

#include <algorithm>

#include "bt_event_queue.h"
#include "blackboard.h"
#include "node.h"
#include "node_condition.h"
#include "types.h"

Pipeline::Pipeline(uint32_t id, std::string name)
    : Composite(id, "Pipeline", std::move(name)) {}

void Pipeline::AddStep(std::unique_ptr<Node> child, int timeout, int retry) {
    Composite::AddChild(std::move(child));  // base: set parent + push children_
    steps_.push_back({timeout, retry});
    retries_used_.push_back(0);
}

NodeStatus Pipeline::Tick(Blackboard& bb, BtEventQueue& events) {
    const size_t n = children_.size();
    if (n == 0 || current_child_index_ >= n) {
        return NodeStatus::kSuccess;  // empty or already complete
    }

    // --- Scan phase (first tick only): start at the first step whose condition
    // already holds. A null condition counts as holding. If none holds, start
    // at step 0 and wait for condition[0].
    if (!started_) {
        bool matched = false;
        for (size_t i = 0; i < n; ++i) {
            NodeCondition* cond = children_[i]->condition();
            if (cond == nullptr) {
                current_child_index_ = i;
                matched = true;
                break;
            }
            NodeStatus s = cond->Eval(bb, events);
            if (s == NodeStatus::kSuccess) {
                current_child_index_ = i;
                matched = true;
                break;
            }
            if (s == NodeStatus::kRunning) {
                return NodeStatus::kRunning;  // re-scan next tick
            }
            // Failure: keep scanning.
        }
        started_ = true;
        if (matched) {
            phase_ = Phase::kAct;  // condition already held → run the action now
        } else {
            current_child_index_ = 0;
            phase_ = Phase::kWait;
            wait_ticks_ = 0;
        }
    }

    // --- Steady state: alternate Wait / Act. A condition that just held or an
    // action that just completed fast-forwards within the same tick.
    while (true) {
        if (phase_ == Phase::kAct) {
            NodeStatus s = children_[current_child_index_]->TickAndRecord(bb, events);
            if (s == NodeStatus::kRunning) return NodeStatus::kRunning;
            if (s == NodeStatus::kFailure) {
                if (!children_[current_child_index_]->last_error().empty()) {
                    set_last_error(children_[current_child_index_]->last_error());
                }
                return NodeStatus::kFailure;
            }
            // Success → advance to the next step and wait for its condition.
            ++current_child_index_;
            if (current_child_index_ >= n) return NodeStatus::kSuccess;
            phase_ = Phase::kWait;
            wait_ticks_ = 0;
            // fall through to the wait this tick
        }

        // phase == kWait: evaluate the current step's condition.
        NodeCondition* cond = children_[current_child_index_]->condition();
        NodeStatus s = (cond == nullptr) ? NodeStatus::kSuccess : cond->Eval(bb, events);
        if (s == NodeStatus::kSuccess) {
            phase_ = Phase::kAct;
            continue;  // run the action this tick
        }
        // Running or Failure → not yet met; spend one wait tick.
        ++wait_ticks_;
        int timeout = steps_[current_child_index_].timeout;
        if (timeout > 0 && wait_ticks_ >= timeout) {
            return OnWaitTimeout(bb, events);
        }
        return NodeStatus::kRunning;
    }
}

NodeStatus Pipeline::OnWaitTimeout(Blackboard& bb, BtEventQueue& events) {
    const size_t i = current_child_index_;
    // No previous step to back up to (step 0), or retry budget exhausted.
    if (i == 0 || retries_used_[i] >= steps_[i].retry) {
        set_last_error("Pipeline '" + name() + "': timed out waiting for step " +
                       std::to_string(i) + " condition");
        return NodeStatus::kFailure;
    }

    // Back up one step: re-check the previous step's condition; if it holds,
    // reset + re-run its action, then resume waiting for this step's condition
    // (with a fresh tick budget) on subsequent ticks.
    ++retries_used_[i];
    current_child_index_ = i - 1;
    NodeCondition* prev_cond = children_[current_child_index_]->condition();
    NodeStatus ps = (prev_cond == nullptr) ? NodeStatus::kSuccess : prev_cond->Eval(bb, events);
    if (ps != NodeStatus::kSuccess) {
        set_last_error("Pipeline '" + name() + "': retry aborted, step " +
                       std::to_string(current_child_index_) + " condition not met");
        return NodeStatus::kFailure;
    }
    children_[current_child_index_]->Reset();  // re-enter the previous action
    phase_ = Phase::kAct;
    return NodeStatus::kRunning;  // tick the re-run action on the next tick
}

void Pipeline::Reset() {
    started_ = false;
    phase_ = Phase::kWait;
    wait_ticks_ = 0;
    std::fill(retries_used_.begin(), retries_used_.end(), 0);
    Composite::Reset();  // current_child_index_ = 0; reset children + conditions
}

void Pipeline::OnAborted() {
    started_ = false;
    phase_ = Phase::kWait;
    wait_ticks_ = 0;
    Composite::OnAborted();
}
