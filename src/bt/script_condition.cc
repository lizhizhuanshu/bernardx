#include "script_condition.h"

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "bt_utils.h"
#include "json_lua.h"
#include "lua_types.h"

ScriptCondition::ScriptCondition(std::string name, std::string script_path,
                                 nlohmann::json params)
    : NodeCondition("Script"),
      name_(std::move(name)),
      script_path_(std::move(script_path)),
      params_json_(std::move(params)) {}

ScriptCondition::~ScriptCondition() {
    if (yielded_co_ != nullptr && host_.lua_context_) {
        host_.lua_context_->CancelCall(yielded_co_);
        yielded_co_ = nullptr;
    }
}

async_simple::coro::Lazy<bool> ScriptCondition::Init(lua_State* L, LuaRuntime* ctx) {
    if (!co_await host_.LoadScript(L, ctx, script_path_, /*require_abort=*/false)) {
        set_last_error(host_.last_error_);
        co_return false;
    }
    // Resolve params now that a lua_State is available (tables -> LuaRef).
    if (params_json_.is_object()) {
        for (auto it = params_json_.begin(); it != params_json_.end(); ++it) {
            args_[it.key()] = JsonToLuaValue(host_.main_L_, ctx, it.value());
        }
        params_json_ = nlohmann::json::object();
    }
    co_return true;
}

NodeStatus ScriptCondition::ParseReturnValue(const LuaValue& v) {
    // Dual mode: a status string selects the state explicitly; any other value
    // is judged by Lua truthiness (false/nil = Failure, else Success).
    if (const auto* s = std::get_if<std::string>(&v)) {
        if (*s == "success") return NodeStatus::kSuccess;
        if (*s == "failure") return NodeStatus::kFailure;
        if (*s == "running") return NodeStatus::kRunning;
        // Unrecognized string falls through to truthiness (non-empty = Success).
    }
    if (std::holds_alternative<std::nullptr_t>(v)) return NodeStatus::kFailure;  // nil
    if (const auto* b = std::get_if<bool>(&v); b && !*b) return NodeStatus::kFailure;
    return NodeStatus::kSuccess;
}

NodeStatus ScriptCondition::HandleResult(const ScriptResult& result) {
    if (result.status != LUA_OK) {
        spdlog::error("ScriptCondition '{}': Tick error: {}", name_, result.error);
        set_last_error("'" + name_ + "' condition error: " + result.error);
        return NodeStatus::kFailure;
    }
    if (result.values.empty()) {
        // No return value is treated as falsy (condition not met).
        return NodeStatus::kFailure;
    }
    return ParseReturnValue(result.values[0]);
}

void ScriptCondition::CallExit(const std::string& reason) {
    if (host_.refs_.exit_ref != LUA_NOREF && host_.main_L_) {
        lua_pushstring(host_.main_L_, reason.c_str());
        LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
    }
}

NodeStatus ScriptCondition::Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) {
    if (!is_loaded() || !host_.main_L_ || !host_.lua_context_) {
        set_last_error("'" + name_ + "' condition not loaded");
        return NodeStatus::kFailure;
    }

    // A previously yielded call has now completed.
    if (yielded_co_ != nullptr) {
        if (!has_result_) return NodeStatus::kRunning;
        auto result = std::move(result_);
        has_result_ = false;
        yielded_co_ = nullptr;

        if (entering_) {
            entering_ = false;
            if (result.status != LUA_OK) {
                spdlog::error("ScriptCondition '{}': Enter error: {}", name_, result.error);
                set_last_error("'" + name_ + "' Enter error: " + result.error);
                return NodeStatus::kFailure;
            }
            // Enter finished — fall through to the first real Tick this tick.
        } else {
            return HandleResult(result);
        }
    }

    // Enter on first tick.
    if (!active_) {
        active_ = true;
        if (host_.refs_.enter_ref != LUA_NOREF) {
            lua_State* co = host_.lua_context_->AcquireCoroutine();
            lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.enter_ref);
            lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.table_ref);
            PushArgsTable(co, args_);

            bool yielded = host_.lua_context_->CallWithCallback(co, 2,
                [this](ScriptResult r) {
                    has_result_ = true;
                    result_ = std::move(r);
                });
            if (yielded) {
                yielded_co_ = co;
                entering_ = true;
                return NodeStatus::kRunning;
            }
            // Enter completed synchronously.
            has_result_ = false;
            if (result_.status != LUA_OK) {
                spdlog::error("ScriptCondition '{}': Enter error: {}", name_, result_.error);
                set_last_error("'" + name_ + "' Enter error: " + result_.error);
                return NodeStatus::kFailure;
            }
        }
    }

    // Run the Tick function.
    lua_State* co = host_.lua_context_->AcquireCoroutine();
    lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.tick_ref);
    lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.table_ref);

    bool yielded = host_.lua_context_->CallWithCallback(co, 1,
        [this](ScriptResult r) {
            has_result_ = true;
            result_ = std::move(r);
        });
    if (yielded) {
        yielded_co_ = co;
        return NodeStatus::kRunning;
    }

    has_result_ = false;
    return HandleResult(result_);
}

void ScriptCondition::Reset() {
    if (yielded_co_ != nullptr && host_.lua_context_) {
        host_.lua_context_->CancelCall(yielded_co_);
        yielded_co_ = nullptr;
        has_result_ = false;
        entering_ = false;
    }
    if (active_) {
        CallExit("reset");
    }
    active_ = false;
}
