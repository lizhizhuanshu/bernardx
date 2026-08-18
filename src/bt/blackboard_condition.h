#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "lua_types.h"
#include "node_condition.h"

class Blackboard;
class BtEventQueue;

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
//   - Synchronous: Tick compares immediately, never returns Running.
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

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;

private:
    static LuaValue ScalarJsonToLuaValue(const nlohmann::json& v);

    std::string key_;
    std::string key2_;  // non-empty -> right side is blackboard[key2_]
    std::string op_;
    LuaValue rhs_ = LuaValue(nullptr);  // literal right side (key2_ path ignores)
};
