#pragma once

#include <string>
#include <unordered_map>

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

    // Build the Enter params table: starts from `args_` (statically-resolved
    // params) and overlays any `$key` blackboard references (`bb_refs_`), read
    // fresh from `bb` at Enter time. A missing key yields nil (a warned miss,
    // not inserted) — matching the `#path` data-reference miss behavior.
    ArgsMap ResolveArgsForEnter(Blackboard& bb) const;

    std::string script_path_;
    nlohmann::json params_json_;
    ArgsMap args_;
    // Param values that begin with `$` (a blackboard reference). Not in args_;
    // resolved against the blackboard on each Enter. Keyed by the param name.
    std::unordered_map<std::string, BbParamRef> bb_refs_;
    LuaScriptHost host_;

    bool active_ = false;
    bool entering_ = false;

    lua_State* yielded_co_ = nullptr;

    bool has_result_ = false;
    ScriptResult result_;
};
