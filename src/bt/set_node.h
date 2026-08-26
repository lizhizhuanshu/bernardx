#pragma once

#include <string>

#include "bt_utils.h"
#include "leaf.h"
#include "lua_types.h"

struct lua_State;
class LuaRuntime;
struct ScriptResult;

// A built-in action that writes the blackboard — no Lua script needed.
// JSON shape: {"type":"Set","params":{"key":"page","value":"home"}}
//   key    (string, required)  blackboard key to write (may be dotted —
//                              see Blackboard::Store routing)
//   value  (scalar, optional)  string/number/bool; absent writes nil. A
//                              string starting with '$' is a blackboard
//                              reference ('$src' copies blackboard[src] at
//                              each Tick; '$$x' escapes to the literal "$x").
//   remove  (bool, optional)  true = delete the key outright (DSL `set k = nil`)
//                             instead of writing a value — Has(key) turns false.
//                             Literal keys only, same semantics as Lua bb.remove.
// Writes through a PROVIDER (a `$src` read from a provider key, or a
// dotted write into a provider root) run the provider's get/set as a
// coroutine: the node returns Running while that is in flight and Success
// once it completes. All provider-free paths stay fully synchronous
// (Success in one Tick). '$src' with a missing/nil source writes nil with
// a warning (consistent with `$key` params resolution).
class SetNode : public Leaf {
public:
    // Literal form: writes `value` every Tick.
    SetNode(uint32_t id, std::string name, std::string key, LuaValue value);

    // Reference form: writes blackboard[ref_key] (read fresh) every Tick.
    SetNode(uint32_t id, std::string name, std::string key, BbParamRef ref);

    // Remove form: deletes key_ every Tick (`set 键 = nil` clear semantics).
    struct RemoveTag {};
    SetNode(uint32_t id, std::string name, std::string key, RemoveTag);

    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;
    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

private:
    // One possibly-suspended provider call (mirrors the ScriptNode
    // yielded_co_/has_result_/result_ pattern).
    struct PendingCall {
        lua_State* co = nullptr;  // outstanding coroutine (null = not pending)
        bool done = false;
        ScriptResult result;
    };

    // Stage `value` into key_: direct store, or a provider set that may
    // suspend (Running until its callback fires).
    NodeStatus DoWrite(Blackboard& bb, const LuaValue& value);
    void CancelPending();

    std::string key_;
    LuaValue value_ = LuaValue(nullptr);  // literal (ref form ignores)
    std::string ref_key_;                 // non-empty -> read bb[ref_key_] fresh
    bool remove_ = false;                 // true -> Blackboard::Remove every Tick

    LuaRuntime* lua_ctx_ = nullptr;  // captured at Init (provider calls)
    PendingCall src_;                // pending $src provider read
    PendingCall write_;              // pending provider write
};
