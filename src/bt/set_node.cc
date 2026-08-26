#include "set_node.h"

#include <utility>

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "lua_runtime.h"
#include "provider_call.h"
#include "types.h"

SetNode::SetNode(uint32_t id, std::string name, std::string key, LuaValue value)
    : Leaf(id, "Set", std::move(name)),
      key_(std::move(key)),
      value_(std::move(value)) {}

SetNode::SetNode(uint32_t id, std::string name, std::string key, BbParamRef ref)
    : Leaf(id, "Set", std::move(name)),
      key_(std::move(key)),
      ref_key_(std::move(ref.key)) {}

SetNode::SetNode(uint32_t id, std::string name, std::string key, RemoveTag)
    : Leaf(id, "Set", std::move(name)),
      key_(std::move(key)),
      remove_(true) {}

async_simple::coro::Lazy<bool> SetNode::Init(lua_State* /*L*/, LuaRuntime* ctx) {
    lua_ctx_ = ctx;  // backs provider get/set coroutines
    co_return true;
}

void SetNode::CancelPending() {
    if (!lua_ctx_) {
        src_ = {};
        write_ = {};
        return;
    }
    if (src_.co != nullptr) {
        lua_ctx_->CancelCall(src_.co);
        src_ = {};
    }
    if (write_.co != nullptr) {
        lua_ctx_->CancelCall(write_.co);
        write_ = {};
    }
}

NodeStatus SetNode::DoWrite(Blackboard& bb, const LuaValue& value) {
    auto w = bb.Store(key_, value);
    if (w.kind != BbWriteResult::Kind::kProvider) {
        // kStored or kRejected (mid-path miss already warned): done.
        return NodeStatus::kSuccess;
    }
    if (!lua_ctx_) {
        spdlog::warn("Set '{}': provider write to '{}' has no LuaRuntime; dropped",
                     name(), key_);
        return NodeStatus::kSuccess;
    }
    auto out = provider_call::InvokeProviderSet(
        lua_ctx_, w.provider, w.path, value,
        [this](ScriptResult r) {
            write_.done = true;
            write_.result = std::move(r);
        });
    if (out.yielded) {
        write_.co = out.co;
        return NodeStatus::kRunning;  // Success once the provider set finishes
    }
    // Completed synchronously (callback already fired). A "no set
    // function" provider reports LUA_ERRRUN — surface it as a warning.
    if (write_.result.status != LUA_OK) {
        spdlog::warn("Set '{}': provider rejected write to '{}': {}",
                     name(), key_, write_.result.error);
        write_ = {};
    }
    return NodeStatus::kSuccess;
}

NodeStatus SetNode::Tick(Blackboard& bb, BtEventQueue& /*events*/) {
    // A pending provider write completed -> the Set is done.
    if (write_.co != nullptr) {
        if (!write_.done) return NodeStatus::kRunning;
        write_ = {};
        return NodeStatus::kSuccess;
    }
    // A pending $src provider read completed -> stage the value and write.
    if (src_.co != nullptr) {
        if (!src_.done) return NodeStatus::kRunning;
        LuaValue v = (!src_.result.values.empty())
                         ? std::move(src_.result.values[0])
                         : LuaValue(nullptr);
        src_ = {};
        return DoWrite(bb, v);
    }

    // Remove form (`set 键 = nil`): delete the key outright - a no-op if
    // absent. Literal keys only, same as Lua bb.remove. Remove never has a
    // pending call, so it lives above the pending checks (unreachable there).
    if (remove_) {
        bb.Remove(key_);
        return NodeStatus::kSuccess;
    }

    // Fresh tick: obtain the value to write.
    if (ref_key_.empty()) {
        return DoWrite(bb, value_);
    }
    // Reference form: read the source fresh on EVERY tick (a Repeat/Retry
    // wrapper re-executes the copy with the latest value).
    auto look = bb.Lookup(ref_key_);
    switch (look.kind) {
    case BbReadResult::Kind::kValue:
        return DoWrite(bb, std::move(look.value));
    case BbReadResult::Kind::kMissing:
        spdlog::warn("Set '{}': reference source key '{}' not set at Tick; writing nil",
                     name(), ref_key_);
        return DoWrite(bb, LuaValue(nullptr));
    case BbReadResult::Kind::kProvider: {
        if (!lua_ctx_) {
            spdlog::warn("Set '{}': provider source '{}' has no LuaRuntime; writing nil",
                         name(), ref_key_);
            return DoWrite(bb, LuaValue(nullptr));
        }
        auto out = provider_call::InvokeProviderGet(
            lua_ctx_, look.provider, look.path,
            [this](ScriptResult r) {
                src_.done = true;
                src_.result = std::move(r);
            });
        if (out.yielded) {
            src_.co = out.co;
            return NodeStatus::kRunning;
        }
        // Synchronous provider completion: the callback already fired.
        LuaValue v = (!src_.result.values.empty())
                         ? std::move(src_.result.values[0])
                         : LuaValue(nullptr);
        src_ = {};
        return DoWrite(bb, std::move(v));
    }
    }
    return NodeStatus::kSuccess;  // unreachable
}

void SetNode::Reset() {
    CancelPending();
    Node::Reset();
}

void SetNode::OnAborted() {
    CancelPending();
    Node::OnAborted();
}
