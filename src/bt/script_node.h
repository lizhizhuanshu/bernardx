#pragma once

#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "leaf.h"
#include "lua_runtime.h"

class ScriptNode : public Leaf {
public:
    ScriptNode(uint32_t id, std::string name, std::string script_path);
    ~ScriptNode() override;

    const std::string& script_path() const { return script_path_; }

    // Initialize: load script file, extract function refs (called on event loop thread)
    // base_path is the BT project root for resolving relative script paths
    // Now async — the script can yield (e.g., require async modules)
    async_simple::coro::Lazy<void> Init(lua_State* L, LuaRuntime* ctx, const std::string& base_path);

    bool is_loaded() const { return tick_ref_ != LUA_NOREF; }

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

    bool is_active() const { return active_; }

private:
    // Synchronous call via lua_pcall (for Enter/Exit/Abort — no yield)
    void CallSync(lua_State* L, int fn_ref, int nargs = 0);

    // Parse status string from coroutine return values.
    // Returns parsed NodeStatus, or kFailure if invalid.
    // Sets `deactivate` to true for terminal statuses (success/failure).
    NodeStatus ParseReturnValues(const std::vector<LuaValue>& values, bool& deactivate);

    std::string script_path_;
    lua_State* main_L_ = nullptr;
    LuaRuntime* lua_context_ = nullptr;

    int enter_ref_ = LUA_NOREF;
    int tick_ref_ = LUA_NOREF;
    int exit_ref_ = LUA_NOREF;
    int abort_ref_ = LUA_NOREF;

    bool active_ = false;

    // Yielded coroutine state (managed by LuaRuntime's coroutine pool)
    lua_State* yielded_co_ = nullptr;

    // Result from coroutine completion callback
    bool has_result_ = false;
    ScriptResult result_;
};
