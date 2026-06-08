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
    if (yielded_co_ != nullptr && host_.lua_context_) {
        host_.lua_context_->CancelCall(yielded_co_);
        yielded_co_ = nullptr;
    }
}

async_simple::coro::Lazy<bool> ScriptNode::Init(lua_State* L, LuaRuntime* ctx, const std::string& base_path) {
    if (!co_await host_.LoadScript(L, ctx, base_path, script_path_, true)) {
        set_last_error(host_.last_error_);
        co_return false;
    }
    co_return true;
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
        std::string msg = "'" + name_ + "' coroutine error: " + result.error;
        spdlog::error("ScriptNode::Tick: {}", msg);
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
        std::string msg = "'" + name_ + "' Enter error: " + result.error;
        spdlog::error("ScriptNode::Tick: {}", msg);
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

    // Enter on first tick
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
