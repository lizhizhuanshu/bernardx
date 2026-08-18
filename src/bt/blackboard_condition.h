#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "lua_types.h"
#include "node_condition.h"

struct lua_State;
class Blackboard;
class BtEventQueue;
class LuaRuntime;
struct ScriptResult;

// A built-in condition that compares a blackboard value against either a
// literal or another blackboard value — no Lua script needed. JSON shape
// (inside a node's `condition` object; fields live under "params"):
//   {"type":"Blackboard","params":{"key":"page","op":"==","value":"home"}}  key vs literal
//   {"type":"Blackboard","params":{"key":"hp","op":">","key2":"shield"}}    key vs key
//   params.key   (string, required)  blackboard key to read (left side);
//                                    supports dotted paths ("proxy.port")
//   params.key2  (string, optional)  blackboard key for the right side — mutually
//                                    exclusive with `value`; both sides are read
//                                    fresh on EVERY Tick (live comparison)
//   params.op    (string, optional)  one of "==", "!=", ">", ">=", "<", "<=",
//                                    "exists" (default "=="; "exists" uses only
//                                    `key` and ignores the right side)
//   params.value (scalar, optional)  expected literal — string/number/bool/null
//
// Semantics:
//   - A PROVIDER-served side (either key) runs the provider's get as a
//     coroutine: both sides launch in parallel, Tick returns Running until
//     both complete, then the comparison runs. Non-provider sides are
//     fully synchronous (the verdict lands in one Tick). While a re-eval
//     is in flight, guards see the last terminal verdict
//     (stale-while-running, like Script conditions).
//   - Missing key (either side) -> Failure (not met) for every op — including
//     "!=". Use "exists" to test presence explicitly.
//   - "==" / "!=" compare type-aware: numbers compare numerically across
//     int/float, strings and bools compare like-for-like, null matches nil.
//   - Ordering ops (">", ">=", "<", "<=") need both sides numeric (or both
//     strings, lexicographic); anything else is Failure with a last_error.
class BlackboardCondition : public NodeCondition {
public:
    // key vs literal: `value` is a JSON scalar (string/number/bool/null).
    BlackboardCondition(std::string key, std::string op, nlohmann::json value);

    // key vs key: the right side is read from blackboard[key2] on each Tick.
    BlackboardCondition(std::string key, std::string op, std::string key2);

    ~BlackboardCondition() override;

    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;
    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

private:
    static LuaValue ScalarJsonToLuaValue(const nlohmann::json& v);

    // One possibly-suspended provider read (mirrors the ScriptNode
    // yielded_co_/has_result_/result_ pattern).
    struct PendingCall {
        lua_State* co = nullptr;  // outstanding coroutine (null = not pending)
        bool done = false;
        ScriptResult result;
    };

    // Read one comparison side. Returns true when a provider coroutine is
    // in flight (pend.co set); otherwise `out` holds the value (or nullopt
    // for a miss/error).
    bool StartRead(Blackboard& bb, const std::string& key,
                   std::optional<LuaValue>& out, PendingCall& pend);
    void CancelPending();
    // The comparison itself (both sides resolved). Clears per-eval state.
    NodeStatus Compare();

    std::string key_;
    std::string key2_;  // non-empty -> right side is blackboard[key2_]
    std::string op_;
    LuaValue rhs_ = LuaValue(nullptr);  // literal right side (key2_ path ignores)

    LuaRuntime* lua_ctx_ = nullptr;  // captured at Init (provider calls)
    PendingCall lhs_pend_;
    PendingCall rhs_pend_;
    std::optional<LuaValue> lhs_val_;
    std::optional<LuaValue> rhs_val_;
};
