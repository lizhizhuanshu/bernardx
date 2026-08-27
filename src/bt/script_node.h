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
#include "leaf.h"
#include "lua_script_host.h"
#include "lua_runtime.h"

class ScriptNode : public Leaf {
public:
    using ArgsMap = std::unordered_map<std::string, LuaValue>;

    // `params` is the raw JSON params object; scalars and tables alike are
    // resolved to LuaValues at Init() (when a lua_State is available).
    ScriptNode(uint32_t id, std::string name, std::string script_path,
               nlohmann::json params = nlohmann::json::object());
    ~ScriptNode() override;

    const std::string& script_path() const { return script_path_; }

    // `*response` (human-like reaction latency, ms): waits this long BEFORE
    // performing the action on its first tick, then re-checks (see
    // SetRecheckCondition) that the triggering condition still holds before
    // actually running. lo==hi fixes the value; lo<hi rolls [lo,hi] per run.
    // Unset (0,0) = no delay (fully backward compatible).
    void SetActionResponse(int lo, int hi);

    // External condition (the Pipeline injects the PRECEDING step's `*target`)
    // re-checked after the `*response` delay. If it (or this action's own
    // `when` guard) no longer holds when the delay elapses, the action does
    // NOT run and Tick returns kFailure so the Pipeline re-verifies/redoes the
    // regressed precondition. null = no re-check (still delays when set).
    void SetRecheckCondition(std::shared_ptr<NodeCondition> c) {
        recheck_ = std::move(c);
    }

    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx) override;

    bool is_loaded() const { return host_.is_loaded(); }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

private:
    NodeStatus ParseReturnValues(const std::vector<LuaValue>& values, bool& deactivate);
    void CallExit(const std::string& reason);
    NodeStatus HandleEnterResult(const ScriptResult& result);
    NodeStatus HandleScriptResult(const ScriptResult& result);

    // Pre-Enter `$key` param resolution. All refs resolve (in parallel)
    // BEFORE Enter runs: static values fill immediately; provider-served
    // refs launch provider.get coroutines and the node stays Running until
    // every one completes — then Enter receives the full args table.
    // One possibly-suspended provider read per param.
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

    std::string script_path_;
    nlohmann::json params_json_;
    ArgsMap args_;

    // `*response` reaction-latency gate (see SetActionResponse). Fixed value or
    // [lo,hi] rolled once per run; cur_response_ms_ is the resolved value for
    // this run (-1 = unresolved). The delay is consumed the first time the
    // node is ticked for a given run; recheck_ (the Pipeline-injected preceding
    // step `*target`) plus this node's own `when` guard are re-verified before
    // the action actually runs.
    int response_lo_ms_ = 0;
    int response_hi_ms_ = 0;
    std::mt19937 rng_{std::random_device{}()};
    int cur_response_ms_ = -1;       // resolved per run; -1 = not yet resolved
    bool response_waiting_ = false;  // a response delay is in progress
    int64_t response_wait_start_ms_ = 0;
    std::shared_ptr<NodeCondition> recheck_;  // injected preceding step `*target`
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
