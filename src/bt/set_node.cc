#include "set_node.h"

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "types.h"

SetNode::SetNode(uint32_t id, std::string name, std::string key, LuaValue value)
    : Leaf(id, "Set", std::move(name)),
      key_(std::move(key)),
      value_(std::move(value)) {}

SetNode::SetNode(uint32_t id, std::string name, std::string key, BbParamRef ref)
    : Leaf(id, "Set", std::move(name)),
      key_(std::move(key)),
      ref_key_(std::move(ref.key)) {}

NodeStatus SetNode::Tick(Blackboard& bb, BtEventQueue& /*events*/) {
    if (ref_key_.empty()) {
        bb.Set(key_, value_);
        return NodeStatus::kSuccess;
    }
    // Reference form: read the source fresh on EVERY tick (a Repeat/Retry
    // wrapper re-executes the copy with the latest value).
    auto src = bb.Get(ref_key_);
    if (!src.has_value()) {
        spdlog::warn("Set '{}': reference source key '{}' not set at Tick; writing nil",
                     name_, ref_key_);
        bb.Set(key_, LuaValue(nullptr));
        return NodeStatus::kSuccess;
    }
    bb.Set(key_, std::move(*src));
    return NodeStatus::kSuccess;
}
