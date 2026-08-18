#include "script_node.h"

#include <filesystem>
#include <utility>

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "bt_event_queue.h"
#include "bt_utils.h"
#include "json_lua.h"
#include "provider_call.h"
#include "types.h"

namespace {

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

std::string BuildErrorJson(const std::string& node, const std::string& phase, const ScriptErrorDetail& detail) {
    return "{\"type\":\"bt_script_error\",\"node\":\"" + EscapeJson(node) +
           "\",\"phase\":\"" + EscapeJson(phase) +
           "\",\"error\":{\"source\":\"" + EscapeJson(detail.source) +
           "\",\"line\":" + std::to_string(detail.line) +
           ",\"message\":\"" + EscapeJson(detail.message) +
           "\",\"stack_trace\":\"" + EscapeJson(detail.stack_trace) + "\"}}";
}

}  // namespace

ScriptNode::ScriptNode(uint32_t id, std::string name, std::string script_path,
                       nlohmann::json params)
    : Leaf(id, "Script", std::move(name)),
      script_path_(std::move(script_path)),
      params_json_(std::move(params)) {}

ScriptNode::~ScriptNode() {
    CancelParamResolution();
    if (yielded_co_ != nullptr && host_.lua_context_) {
        host_.lua_context_->CancelCall(yielded_co_);
        yielded_co_ = nullptr;
    }
}

async_simple::coro::Lazy<bool> ScriptNode::Init(lua_State* L, LuaRuntime* ctx) {
    if (!co_await host_.LoadScript(L, ctx, script_path_, true)) {
        set_last_error(host_.last_error_);
        co_return false;
    }
    // Resolve params now that a lua_State is available: scalars map directly,
    // objects/arrays become Lua tables (LuaRef on host_.main_L_, shared across
    // coroutines via the registry). A string value beginning with `$` is a
    // blackboard reference (`$key`) or an escaped literal (`$$..`) — those are
    // NOT resolved here; refs are read fresh from the blackboard at Enter time.
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
        params_json_ = nlohmann::json::object();  // release raw JSON
    }
    co_return true;
}

void ScriptNode::StartParamResolution(Blackboard& bb) {
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
            spdlog::warn("ScriptNode '{}': blackboard param '{}' key '{}' not set at Enter",
                         name_, param_name, ref.key);
            break;
        case BbReadResult::Kind::kProvider: {
            if (!host_.lua_context_) {
                spdlog::warn("ScriptNode '{}': blackboard param '{}' key '{}' has no LuaRuntime",
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

void ScriptNode::CollectResolved() {
    for (size_t k = 0; k < pending_params_.size(); ++k) {
        const auto& p = pending_params_[k];
        if (!p.result.values.empty()) {
            resolved_[pending_names_[k]] = std::move(p.result.values[0]);
        } else {
            // A provider error or nil return: a warned miss, not inserted.
            const auto it = bb_refs_.find(pending_names_[k]);
            spdlog::warn("ScriptNode '{}': blackboard param '{}' key '{}' not set at Enter",
                         name_, pending_names_[k],
                         it != bb_refs_.end() ? it->second.key : "?");
        }
    }
    pending_params_.clear();
    pending_names_.clear();
}

void ScriptNode::CancelParamResolution() {
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

ScriptNode::ArgsMap ScriptNode::EnterArgs() const {
    ArgsMap enter_args = args_;
    for (const auto& [name, value] : resolved_) {
        enter_args[name] = value;
    }
    return enter_args;
}

NodeStatus ScriptNode::ParseReturnValues(const std::vector<LuaValue>& values, bool& deactivate) {
    if (values.empty()) {
        deactivate = true;
        return NodeStatus::kFailure;
    }
    auto* s = std::get_if<std::string>(&values[0]);
    if (!s) {
        std::string msg = "'" + name_ + "' returned unexpected value";
        spdlog::error("ScriptNode::Tick: {}", msg);
        set_last_error(std::move(msg));
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
    std::string msg = "'" + name_ + "' returned unexpected value: '" + *s + "'";
    spdlog::error("ScriptNode::Tick: {}", msg);
    set_last_error(std::move(msg));
    deactivate = true;
    return NodeStatus::kFailure;
}

void ScriptNode::CallExit(const std::string& reason) {
    if (host_.refs_.exit_ref != LUA_NOREF) {
        lua_pushstring(host_.main_L_, reason.c_str());
        LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
    }
}

NodeStatus ScriptNode::HandleScriptResult(const ScriptResult& result) {
    if (result.status != LUA_OK) {
        spdlog::error("{}", BuildErrorJson(name_, "Tick", result.error_detail));
        std::string msg = "'" + name_ + "' (" + script_path_ + ") Tick failed: " +
                          FormatScriptError(result.error_detail);
        set_last_error(msg);
        active_ = false;
        CallExit("failure");
        return NodeStatus::kFailure;
    }

    bool deactivate = false;
    auto ns = ParseReturnValues(result.values, deactivate);
    if (deactivate) {
        active_ = false;
        auto* s = std::get_if<std::string>(&result.values[0]);
        CallExit(s ? *s : "failure");
    }
    return ns;
}

NodeStatus ScriptNode::HandleEnterResult(const ScriptResult& result) {
    if (result.status != LUA_OK) {
        spdlog::error("{}", BuildErrorJson(name_, "Enter", result.error_detail));
        std::string msg = "'" + name_ + "' (" + script_path_ + ") Enter failed: " +
                          FormatScriptError(result.error_detail);
        set_last_error(msg);
        active_ = false;
        CallExit("failure");
        return NodeStatus::kFailure;
    }
    return NodeStatus::kRunning;
}

NodeStatus ScriptNode::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!is_loaded() || !host_.main_L_ || !host_.lua_context_) {
        set_last_error("'" + name_ + "' not loaded");
        return NodeStatus::kFailure;
    }

    // Check if a yielded call has completed
    if (yielded_co_ != nullptr) {
        if (!has_result_) {
            return NodeStatus::kRunning;
        }
        auto result = std::move(result_);
        has_result_ = false;
        yielded_co_ = nullptr;

        if (entering_) {
            entering_ = false;
            auto status = HandleEnterResult(result);
            if (status != NodeStatus::kRunning) return status;
        } else {
            return HandleScriptResult(result);
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

            // Enter completed synchronously
            has_result_ = false;
            auto status = HandleEnterResult(result_);
            if (status != NodeStatus::kRunning) return status;
        }
    }

    // Start the tick call
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

    // Completed synchronously — callback already fired, result_ is set
    has_result_ = false;
    return HandleScriptResult(result_);
}

void ScriptNode::Reset() {
    if (yielded_co_ != nullptr) {
        host_.lua_context_->CancelCall(yielded_co_);
        yielded_co_ = nullptr;
        has_result_ = false;
        entering_ = false;
    }
    CancelParamResolution();  // also resets enter_stage_ to kIdle

    if (active_ && host_.refs_.exit_ref != LUA_NOREF && host_.main_L_) {
        lua_pushstring(host_.main_L_, "reset");
        LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
    }
    active_ = false;
    Leaf::Reset();
}

void ScriptNode::OnAborted() {
    if (yielded_co_ != nullptr) {
        host_.lua_context_->CancelCall(yielded_co_);
        yielded_co_ = nullptr;
        has_result_ = false;
        entering_ = false;
    }
    CancelParamResolution();  // also resets enter_stage_ to kIdle

    if (active_ && host_.main_L_) {
        if (host_.refs_.abort_ref != LUA_NOREF) {
            LuaCallMethod(host_.main_L_, host_.refs_.abort_ref, host_.refs_.table_ref, 0);
        }
        if (host_.refs_.exit_ref != LUA_NOREF) {
            lua_pushstring(host_.main_L_, "aborted");
            LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
        }
    }
    active_ = false;
    Leaf::OnAborted();
}
