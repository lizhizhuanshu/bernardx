#include "script_condition.h"

#include <utility>

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "bt_utils.h"
#include "json_lua.h"
#include "lua_types.h"
#include "provider_call.h"

ScriptCondition::ScriptCondition(std::string name, std::string script_path,
                                 nlohmann::json params)
    : NodeCondition("Script"),
      name_(std::move(name)),
      script_path_(std::move(script_path)),
      params_json_(std::move(params)) {}

ScriptCondition::~ScriptCondition() {
    CancelParamResolution();
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
    // Resolve params now that a lua_State is available (tables -> LuaRef). A
    // string value beginning with `$` is a blackboard reference (`$key`) or an
    // escaped literal (`$$..`); refs are read fresh from the blackboard at Enter.
    if (params_json_.is_object()) {
        for (auto it = params_json_.begin(); it != params_json_.end(); ++it) {
            if (it.value().is_string()) {
                auto cls = ResolveBbParamMarker(it.value().get_ref<const std::string&>());
                if (auto* ref = std::get_if<BbParamRef>(&cls)) {
                    bb_refs_[it.key()] = std::move(*ref);
                    continue;
                }
                if (auto* lit = std::get_if<std::string>(&cls)) {
                    args_[it.key()] = LuaValue(*lit);
                    continue;
                }
            }
            args_[it.key()] = JsonToLuaValue(host_.main_L_, ctx, it.value());
        }
        params_json_ = nlohmann::json::object();
    }
    co_return true;
}

void ScriptCondition::StartParamResolution(Blackboard& bb) {
    resolved_.clear();
    pending_params_.clear();
    pending_names_.clear();
    for (const auto& [param_name, ref] : bb_refs_) {
        auto look = bb.Lookup(ref.key);
        switch (look.kind) {
        case BbReadResult::Kind::kValue:
            resolved_[param_name] = std::move(look.value);
            break;
        case BbReadResult::Kind::kMissing:
            spdlog::warn("ScriptCondition '{}': blackboard param '{}' key '{}' not set at Enter",
                         name_, param_name, ref.key);
            break;
        case BbReadResult::Kind::kProvider: {
            if (!host_.lua_context_) {
                spdlog::warn("ScriptCondition '{}': blackboard param '{}' key '{}' has no LuaRuntime",
                             name_, param_name, ref.key);
                break;
            }
            // Slot first, launch second: the callback indexes the slot, so
            // a synchronous completion lands in the already-pushed element.
            pending_names_.push_back(param_name);
            pending_params_.push_back({});
            size_t idx = pending_params_.size() - 1;
            auto out = provider_call::InvokeProviderGet(
                host_.lua_context_, look.provider, look.path,
                [this, idx](ScriptResult r) {
                    pending_params_[idx].done = true;
                    pending_params_[idx].result = std::move(r);
                });
            if (out.yielded) {
                pending_params_[idx].co = out.co;
            }
            break;
        }
        }
    }
}

void ScriptCondition::CollectResolved() {
    for (size_t k = 0; k < pending_params_.size(); ++k) {
        const auto& p = pending_params_[k];
        if (!p.result.values.empty()) {
            resolved_[pending_names_[k]] = std::move(p.result.values[0]);
        } else {
            // A provider error or nil return: a warned miss, not inserted.
            const auto it = bb_refs_.find(pending_names_[k]);
            spdlog::warn("ScriptCondition '{}': blackboard param '{}' key '{}' not set at Enter",
                         name_, pending_names_[k],
                         it != bb_refs_.end() ? it->second.key : "?");
        }
    }
    pending_params_.clear();
    pending_names_.clear();
}

void ScriptCondition::CancelParamResolution() {
    if (host_.lua_context_) {
        for (auto& p : pending_params_) {
            if (p.co != nullptr) host_.lua_context_->CancelCall(p.co);
        }
    }
    pending_params_.clear();
    pending_names_.clear();
    resolved_.clear();
    enter_stage_ = EnterStage::kIdle;
}

ScriptCondition::ArgsMap ScriptCondition::EnterArgs() const {
    ArgsMap enter_args = args_;
    for (const auto& [name, value] : resolved_) {
        enter_args[name] = value;
    }
    return enter_args;
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
        set_last_error("'" + name_ + "' (" + script_path_ +
                       ") condition Tick failed: " + FormatScriptError(result.error_detail));
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

NodeStatus ScriptCondition::Tick(Blackboard& bb, BtEventQueue& /*events*/) {
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
                set_last_error("'" + name_ + "' (" + script_path_ +
                               ") condition Enter failed: " + FormatScriptError(result.error_detail));
                return NodeStatus::kFailure;
            }
            // Enter finished — fall through to the first real Tick this tick.
        } else {
            return HandleResult(result);
        }
    }

    // Enter on first tick — after the `$key` params fully resolve.
    if (!active_) {
        if (enter_stage_ == EnterStage::kIdle) {
            enter_stage_ = EnterStage::kResolving;
            if (host_.refs_.enter_ref != LUA_NOREF && !bb_refs_.empty()) {
                StartParamResolution(bb);
            }
        }
        if (enter_stage_ == EnterStage::kResolving) {
            if (!pending_params_.empty()) {
                for (const auto& p : pending_params_) {
                    if (!p.done) return NodeStatus::kRunning;
                }
                CollectResolved();
            }
            enter_stage_ = EnterStage::kReady;
        }
        active_ = true;
        if (host_.refs_.enter_ref != LUA_NOREF) {
            lua_State* co = host_.lua_context_->AcquireCoroutine();
            lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.enter_ref);
            lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.table_ref);
            PushArgsTable(co, EnterArgs());

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
                set_last_error("'" + name_ + "' (" + script_path_ +
                               ") condition Enter failed: " + FormatScriptError(result_.error_detail));
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
    CancelParamResolution();  // also resets enter_stage_ to kIdle
    if (active_) {
        CallExit("reset");
    }
    active_ = false;
}
