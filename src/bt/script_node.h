#pragma once

#include <string>
#include <unordered_map>

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

    ScriptNode(uint32_t id, std::string name, std::string script_path, ArgsMap args = {});
    ~ScriptNode() override;

    const std::string& script_path() const { return script_path_; }

    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx, const std::string& base_path) override;

    bool is_loaded() const { return host_.is_loaded(); }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

private:
    NodeStatus ParseReturnValues(const std::vector<LuaValue>& values, bool& deactivate);

    std::string script_path_;
    ArgsMap args_;
    LuaScriptHost host_;

    bool active_ = false;

    lua_State* yielded_co_ = nullptr;

    bool has_result_ = false;
    ScriptResult result_;
};
