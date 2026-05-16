#include "script_node.h"

#include <filesystem>
#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "types.h"

ScriptNode::ScriptNode(uint32_t id, std::string name, std::string script_path)
    : Leaf(id, "Script", std::move(name)),
      script_path_(std::move(script_path)) {}

void ScriptNode::Init(lua_State* L, LuaContext* ctx, const std::string& base_path) {
    main_L_ = L;
    lua_context_ = ctx;

    std::string full_path = script_path_;
    if (!base_path.empty() && !std::filesystem::path(script_path_).is_absolute()) {
        full_path = std::filesystem::absolute(base_path + "/" + script_path_).string();
    }

    if (luaL_loadfile(L, full_path.c_str()) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("ScriptNode::Init: failed to load '{}': {}", full_path, err ? err : "unknown");
        lua_pop(L, 1);
        return;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("ScriptNode::Init: failed to execute '{}': {}", full_path, err ? err : "unknown");
        lua_pop(L, 1);
        return;
    }

    if (!lua_istable(L, -1)) {
        spdlog::error("ScriptNode::Init: '{}' did not return a table", full_path);
        lua_pop(L, 1);
        return;
    }

    int table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto get_ref = [&](const char* name) -> int {
        lua_rawgeti(L, LUA_REGISTRYINDEX, table_ref);
        lua_getfield(L, -1, name);
        int ref = LUA_NOREF;
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -1);
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        lua_pop(L, 2);
        return ref;
    };

    enter_ref_ = get_ref("Enter");
    tick_ref_ = get_ref("Tick");
    exit_ref_ = get_ref("Exit");
    abort_ref_ = get_ref("Abort");

    luaL_unref(L, LUA_REGISTRYINDEX, table_ref);

    if (tick_ref_ == LUA_NOREF) {
        spdlog::error("ScriptNode::Init: '{}' missing required 'Tick' function", script_path_);
    }

    spdlog::info("ScriptNode::Init: loaded '{}' (Enter={}, Tick={}, Exit={}, Abort={})",
                 script_path_,
                 enter_ref_ != LUA_NOREF, tick_ref_ != LUA_NOREF,
                 exit_ref_ != LUA_NOREF, abort_ref_ != LUA_NOREF);
}

void ScriptNode::CallSync(lua_State* L, int fn_ref, int nargs) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    if (nargs > 0) {
        lua_insert(L, -(nargs + 1));
    }
    int status = lua_pcall(L, nargs, 0, 0);
    if (status != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("ScriptNode::CallSync: error: {}", err ? err : "unknown");
        lua_pop(L, 1);
    }
}

NodeStatus ScriptNode::ParseReturnValues(const std::vector<LuaValue>& values, bool& deactivate) {
    if (values.empty()) {
        deactivate = true;
        return NodeStatus::kFailure;
    }
    auto* s = std::get_if<std::string>(&values[0]);
    if (!s) {
        spdlog::error("ScriptNode::Tick: '{}' returned unexpected value", name_);
        deactivate = true;
        return NodeStatus::kFailure;
    }
    if (*s == "success" || *s == "failure") {
        deactivate = true;
        return (*s == "success") ? NodeStatus::kSuccess : NodeStatus::kFailure;
    }
    if (*s == "running") {
        return NodeStatus::kRunning;
    }
    spdlog::error("ScriptNode::Tick: '{}' returned unexpected value", name_);
    deactivate = true;
    return NodeStatus::kFailure;
}

NodeStatus ScriptNode::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!is_loaded() || !main_L_ || !lua_context_) {
        return NodeStatus::kFailure;
    }

    // --- Waiting for a yielded coroutine to complete ---
    if (yielded_co_ != nullptr) {
        if (!has_result_) {
            return NodeStatus::kRunning;  // Coroutine still yielded
        }
        auto result = std::move(result_);
        has_result_ = false;
        yielded_co_ = nullptr;

        if (result.status != LUA_OK) {
            spdlog::error("ScriptNode::Tick: '{}' coroutine error: {}", name_, result.error);
            active_ = false;
            return NodeStatus::kFailure;
        }

        bool deactivate = false;
        auto ns = ParseReturnValues(result.values, deactivate);
        if (deactivate) {
            active_ = false;
            if (exit_ref_ != LUA_NOREF) {
                auto* s = std::get_if<std::string>(&result.values[0]);
                lua_pushstring(main_L_, s ? s->c_str() : "failure");
                CallSync(main_L_, exit_ref_, 1);
            }
        }
        return ns;
    }

    // --- Start a new tick ---
    if (!active_) {
        active_ = true;
        if (enter_ref_ != LUA_NOREF) {
            bb.PushAsTable(main_L_);
            CallSync(main_L_, enter_ref_, 1);
        }
    }

    // Create coroutine via LuaContext's pool
    lua_State* co = lua_context_->AcquireCoroutine();

    // Push Tick function and blackboard argument on the coroutine
    lua_rawgeti(co, LUA_REGISTRYINDEX, tick_ref_);
    bb.PushAsTable(co);

    // Register completion callback (fires when coroutine finishes after yield)
    lua_context_->SetCoCompleteCallback(co, [this](ScriptResult r) {
        has_result_ = true;
        result_ = std::move(r);
    });

    int nresults = 0;
    int status = lua_resume(co, main_L_, 1, &nresults);

    if (status == LUA_YIELD) {
        yielded_co_ = co;
        return NodeStatus::kRunning;
    }

    // Coroutine finished immediately (no yield)
    lua_context_->RemoveCoCompleteCallback(co);

    if (status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        spdlog::error("ScriptNode::Tick: '{}' error: {}", name_, err ? err : "unknown");
        lua_pop(co, 1);
        active_ = false;
        return NodeStatus::kFailure;
    }

    auto values = LuaContext::PeekValues(co, nresults);
    lua_pop(co, nresults);

    bool deactivate = false;
    auto ns = ParseReturnValues(values, deactivate);
    if (deactivate) {
        active_ = false;
        if (exit_ref_ != LUA_NOREF) {
            auto* s = std::get_if<std::string>(&values[0]);
            lua_pushstring(main_L_, s ? s->c_str() : "failure");
            CallSync(main_L_, exit_ref_, 1);
        }
    }
    return ns;
}

void ScriptNode::Reset() {
    if (yielded_co_ != nullptr) {
        lua_context_->RemoveCoCompleteCallback(yielded_co_);
        yielded_co_ = nullptr;
        has_result_ = false;
    }

    if (active_ && exit_ref_ != LUA_NOREF && main_L_) {
        lua_pushstring(main_L_, "reset");
        CallSync(main_L_, exit_ref_, 1);
    }
    active_ = false;
    Leaf::Reset();
}

void ScriptNode::OnAborted() {
    if (yielded_co_ != nullptr) {
        lua_context_->RemoveCoCompleteCallback(yielded_co_);
        yielded_co_ = nullptr;
        has_result_ = false;
    }

    if (active_ && main_L_) {
        if (abort_ref_ != LUA_NOREF) {
            CallSync(main_L_, abort_ref_, 0);
        }
        if (exit_ref_ != LUA_NOREF) {
            lua_pushstring(main_L_, "aborted");
            CallSync(main_L_, exit_ref_, 1);
        }
    }
    active_ = false;
    Leaf::OnAborted();
}
