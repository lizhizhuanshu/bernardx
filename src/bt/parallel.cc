#include "parallel.h"

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "node.h"

Parallel::Parallel(uint32_t id, std::string name, Policy success, Policy failure)
    : Composite(id, "Parallel", std::move(name)),
      success_policy_(success),
      failure_policy_(failure) {}

NodeStatus Parallel::Tick(Blackboard& bb, BtEventQueue& events) {
    int success_count = 0;
    int failure_count = 0;
    int running_count = 0;
    int total = static_cast<int>(children_.size());

    for (auto& child : children_) {
        auto status = child->Tick(bb, events);
        switch (status) {
            case NodeStatus::kSuccess:
                ++success_count;
                break;
            case NodeStatus::kFailure:
                if (!child->last_error().empty() && last_error().empty()) {
                    set_last_error(child->last_error());
                }
                ++failure_count;
                break;
            case NodeStatus::kRunning:
                ++running_count;
                break;
        }
    }

    // Check failure condition
    bool failed = (failure_policy_ == Policy::kRequireOne)
                      ? (failure_count > 0)
                      : (failure_count == total);
    if (failed) {
        current_child_index_ = 0;
        return NodeStatus::kFailure;
    }

    // Check success condition
    bool succeeded = (success_policy_ == Policy::kRequireAll)
                         ? (success_count == total)
                         : (success_count > 0);
    if (succeeded) {
        current_child_index_ = 0;
        return NodeStatus::kSuccess;
    }

    return NodeStatus::kRunning;
}
