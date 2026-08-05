#pragma once

#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"
#include "lua_script_host.h"
#include "lua_types.h"
#include "node_condition.h"

class Blackboard;
class BtEventQueue;

// A condition backed by a Lua script. Loads a module table with optional
// `Enter`/`Tick`/`Exit` (the same shape action `ScriptNode` and the old sensor
// scripts use), so existing page-recognition scripts port over unchanged.
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

    std::string name_;
    std::string script_path_;
    nlohmann::json params_json_;
    ArgsMap args_;
    LuaScriptHost host_;

    bool active_ = false;
    bool entering_ = false;

    lua_State* yielded_co_ = nullptr;
    bool has_result_ = false;
    ScriptResult result_;
};
