#pragma once

#include <cstddef>
#include <memory>
#include <vector>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"
#include "node.h"

class ShuffledIndexTracker;  // defined in bt_utils.h; included only where needed

// Sequential composite policy shared by Selector/Sequence (and their random
// variants). Encodes the only two behaviours that differ between those nodes;
// see Composite::TickSequential.
enum class SeqPolicy {
    Selector,  // succeed on first success, skip failures
    Sequence,  // fail on first failure, skip successes
};

class Composite : public Node {
public:
    void AddChild(std::unique_ptr<Node> child);

    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

    bool has_started() const { return current_child_index_ > 0; }
    size_t current_child_index() const { return current_child_index_; }
    void set_current_child_index(size_t idx) { current_child_index_ = idx; }

    void Reset() override;
    void OnAborted() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;

protected:
    Composite(uint32_t id, std::string type, std::string name);

    // Shared tick loop for Selector/Sequence (and random variants). When
    // `tracker` is non-null children are visited in its shuffled order.
    NodeStatus TickSequential(Blackboard& bb, BtEventQueue& events,
                              SeqPolicy policy, ShuffledIndexTracker* tracker = nullptr);

    std::vector<std::unique_ptr<Node>> children_;
    size_t current_child_index_ = 0;
};
