#include "script_node.h"

#include <filesystem>
#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "types.h"

ScriptNode::ScriptNode(uint32_t id, std::string name, std::string script_path, ArgsMap args)
    : Leaf(id, "Script", std::move(name)),
      script_path_(std::move(script_path)),
      args_(std::move(args)) {}

ScriptNode::~ScriptNode() {
    if (yielded_co_ != nullptr && lua_context_ != nullptr) {
        lua_context_->RemoveCoCompleteCallback(yielded_co_);
        yielded_co_ = nullptr;
    }
}

void ScriptNode::ReleaseRefs() {
    if (!main_L_) return;
    auto unref = [&](int& ref) {
        if (ref != LUA_NOREF) {
            luaL_unref(main_L_, LUA_REGISTRYINDEX, ref);
            ref = LUA_NOREF;
        }
    };
    unref(script_table_ref_);
    unref(enter_ref_);
    unref(tick_ref_);
    unref(exit_ref_);
    unref(abort_ref_);
}

async_simple::coro::Lazy<void> ScriptNode::Init(lua_State* L, LuaRuntime* ctx, const std::string& base_path) {
    main_L_ = L;
    lua_context_ = ctx;

    std::string full_path = script_path_;
    if (!base_path.empty() && !std::filesystem::path(script_path_).is_absolute()) {
        full_path = std::filesystem::absolute(base_path + "/" + script_path_).string();
    }

    auto result = co_await ctx->DoFileAsync(full_path);

    if (result.status != LUA_OK) {
        spdlog::error("ScriptNode::Init: failed to execute '{}': {}", full_path,
                      result.error.empty() ? "unknown error" : result.error);
        co_return;
    }

    if (result.values.empty()) {
        spdlog::error("ScriptNode::Init: '{}' did not return a value", full_path);
        co_return;
    }

    auto* table_ref = std::get_if<LuaRef>(&result.values[0]);
    if (!table_ref) {
        spdlog::error("ScriptNode::Init: '{}' did not return a table", full_path);
        co_return;
    }

    lua_rawgeti(main_L_, LUA_REGISTRYINDEX, (*table_ref)->ref);
    if (!lua_istable(main_L_, -1)) {
        spdlog::error("ScriptNode::Init: '{}' did not return a table", full_path);
        lua_pop(main_L_, 1);
        co_return;
    }

    // Save the script table ref for colon-method calls (self)
    lua_pushvalue(main_L_, -1);
    script_table_ref_ = luaL_ref(main_L_, LUA_REGISTRYINDEX);

    int table_idx = lua_absindex(main_L_, -1);

    auto get_ref = [&](const char* name) -> int {
        lua_getfield(main_L_, table_idx, name);
        int ref = LUA_NOREF;
        if (lua_isfunction(main_L_, -1)) {
            lua_pushvalue(main_L_, -1);
            ref = luaL_ref(main_L_, LUA_REGISTRYINDEX);
        }
        lua_pop(main_L_, 1);
        return ref;
    };

    enter_ref_ = get_ref("Enter");
    tick_ref_ = get_ref("Tick");
    exit_ref_ = get_ref("Exit");
    abort_ref_ = get_ref("Abort");

    lua_pop(main_L_, 1);  // pop table

    if (tick_ref_ == LUA_NOREF) {
        spdlog::error("ScriptNode::Init: '{}' missing required 'Tick' function", script_path_);
    }

    spdlog::info("ScriptNode::Init: loaded '{}' (Enter={}, Tick={}, Exit={}, Abort={})",
                 script_path_,
                 enter_ref_ != LUA_NOREF, tick_ref_ != LUA_NOREF,
                 exit_ref_ != LUA_NOREF, abort_ref_ != LUA_NOREF);
}

void ScriptNode::PushArgsTable(lua_State* L) const {
    lua_newtable(L);
    for (const auto& [key, value] : args_) {
        lua_pushstring(L, key.c_str());
        LuaRuntime::PushValues(L, {value});
        lua_settable(L, -3);
    }
}

void ScriptNode::CallMethod(lua_State* L, int fn_ref, int extra_args) {
    // Stack before: [..., extra_arg1, ..., extra_argN]
    // We need:     [..., fn, self, extra_arg1, ..., extra_argN]
    int base = lua_gettop(L) - extra_args + 1;  // first extra_arg position

    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    lua_insert(L, base);  // fn at base

    lua_rawgeti(L, LUA_REGISTRYINDEX, script_table_ref_);
    lua_insert(L, base + 1);  // self at base+1
    // Stack now: [..., fn, self, extra_arg1, ..., extra_argN]

    int status = lua_pcall(L, 1 + extra_args, 0, 0);
    if (status != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("ScriptNode::CallMethod: error: {}", err ? err : "unknown");
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
            return NodeStatus::kRunning;
        }
        auto result = std::move(result_);
        has_result_ = false;
        yielded_co_ = nullptr;

        if (result.status != LUA_OK) {
            spdlog::error("ScriptNode::Tick: '{}' coroutine error: {}", name_, result.error);
            active_ = false;
            if (exit_ref_ != LUA_NOREF) {
                lua_pushstring(main_L_, "failure");
                CallMethod(main_L_, exit_ref_, 1);
            }
            return NodeStatus::kFailure;
        }

        bool deactivate = false;
        auto ns = ParseReturnValues(result.values, deactivate);
        if (deactivate) {
            active_ = false;
            if (exit_ref_ != LUA_NOREF) {
                auto* s = std::get_if<std::string>(&result.values[0]);
                lua_pushstring(main_L_, s ? s->c_str() : "failure");
                CallMethod(main_L_, exit_ref_, 1);
            }
        }
        return ns;
    }

    // --- Start a new tick ---
    if (!active_) {
        active_ = true;
        if (enter_ref_ != LUA_NOREF) {
            PushArgsTable(main_L_);
            CallMethod(main_L_, enter_ref_, 1);
        }
    }

    // Create coroutine via LuaRuntime's pool
    lua_State* co = lua_context_->AcquireCoroutine();

    // Stack: fn, self — lua_resume passes self as the sole argument to fn
    lua_rawgeti(co, LUA_REGISTRYINDEX, tick_ref_);
    lua_rawgeti(co, LUA_REGISTRYINDEX, script_table_ref_);

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

    lua_context_->RemoveCoCompleteCallback(co);

    if (status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        spdlog::error("ScriptNode::Tick: '{}' error: {}", name_, err ? err : "unknown");
        lua_pop(co, 1);
        active_ = false;
        if (exit_ref_ != LUA_NOREF) {
            lua_pushstring(main_L_, "failure");
            CallMethod(main_L_, exit_ref_, 1);
        }
        return NodeStatus::kFailure;
    }

    auto values = LuaRuntime::PeekValues(co, nresults);
    lua_pop(co, nresults);

    bool deactivate = false;
    auto ns = ParseReturnValues(values, deactivate);
    if (deactivate) {
        active_ = false;
        if (exit_ref_ != LUA_NOREF) {
            auto* s = std::get_if<std::string>(&values[0]);
            lua_pushstring(main_L_, s ? s->c_str() : "failure");
            CallMethod(main_L_, exit_ref_, 1);
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
        CallMethod(main_L_, exit_ref_, 1);
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
            CallMethod(main_L_, abort_ref_);
        }
        if (exit_ref_ != LUA_NOREF) {
            lua_pushstring(main_L_, "aborted");
            CallMethod(main_L_, exit_ref_, 1);
        }
    }
    active_ = false;
    Leaf::OnAborted();
}
