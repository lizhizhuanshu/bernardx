#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "bt_utils.h"
#include "lua_runtime.h"
#include "lua_script_host.h"
#include "lua_types.h"
#include "node_condition.h"

class Blackboard;
class BtEventQueue;

// A condition backed by a Lua script. Loads a module table with optional
// `Enter`/`Tick`/`Exit` (the same shape as `ScriptNode`), so existing
// page-recognition scripts port over unchanged.
//
// Lifecycle is simpler than `ScriptNode`: `Enter` runs once on the first
// `Tick`; `Tick` may be called repeatedly (yielding -> Running) until it
// returns a terminal value; `Exit("reset")` runs on `Reset`. The condition
// stays active between terminal results — it is only torn down by `Reset`,
// since a guard can be re-queried across ticks during a Pipeline scan.
//
// Return protocol (dual mode):
//   - status string  -> "success" / "failure" / "running"
//   - any other value -> Lua truthiness (false/nil = Failure, else Success)
class ScriptCondition : public NodeCondition {
public:
    using ArgsMap = std::unordered_map<std::string, LuaValue>;

    // `params` is the raw JSON params object; resolved to LuaValues at Init().
    ScriptCondition(std::string name, std::string script_path,
                    nlohmann::json params = nlohmann::json::object());
    ~ScriptCondition() override;

    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;
    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;

    bool is_loaded() const { return host_.is_loaded(); }

private:
    NodeStatus ParseReturnValue(const LuaValue& v);
    NodeStatus HandleResult(const ScriptResult& result);
    void CallExit(const std::string& reason);

    // Pre-Enter `$key` param resolution. All refs resolve (in parallel)
    // BEFORE Enter runs: static values fill immediately; provider-served
    // refs launch provider.get coroutines and the condition stays Running
    // until every one completes — then Enter receives the full args table.
    // One possibly-suspended provider read (the yielded_co_/has_result_
    // pattern, one slot per param).
    struct PendingParam {
        lua_State* co = nullptr;  // outstanding coroutine (null = done/none)
        bool done = false;
        ScriptResult result;
    };
    void StartParamResolution(Blackboard& bb);  // fills resolved_/pending_
    void CollectResolved();                     // pending_ -> resolved_ (warns misses)
    void CancelParamResolution();
    // args_ overlaid with the resolved `$key` values.
    ArgsMap EnterArgs() const;

    std::string name_;
    std::string script_path_;
    nlohmann::json params_json_;
    ArgsMap args_;
    // Param values that begin with `$` (a blackboard reference). Not in args_;
    // resolved against the blackboard on each Enter. Keyed by the param name.
    std::unordered_map<std::string, BbParamRef> bb_refs_;
    LuaScriptHost host_;

    bool active_ = false;
    bool entering_ = false;
    // Enter sequencing: kIdle -> (resolve params) -> kReady -> Enter.
    enum class EnterStage { kIdle, kResolving, kReady };
    EnterStage enter_stage_ = EnterStage::kIdle;
    std::vector<PendingParam> pending_params_;         // parallel to pending_names_
    std::vector<std::string> pending_names_;
    std::unordered_map<std::string, LuaValue> resolved_;  // completed $key values

    lua_State* yielded_co_ = nullptr;
    bool has_result_ = false;
    ScriptResult result_;
};
