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
        host_.lua_context_->RemoveCoCompleteCallback(yielded_co_);
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

NodeStatus ScriptNode::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!is_loaded() || !host_.main_L_ || !host_.lua_context_) {
        set_last_error("'" + name_ + "' not loaded");
        return NodeStatus::kFailure;
    }

    if (yielded_co_ != nullptr) {
        if (!has_result_) {
            return NodeStatus::kRunning;
        }
        auto result = std::move(result_);
        has_result_ = false;
        yielded_co_ = nullptr;

        if (result.status != LUA_OK) {
            std::string msg = "'" + name_ + "' coroutine error: " + result.error;
            spdlog::error("ScriptNode::Tick: {}", msg);
            set_last_error(std::move(msg));
            active_ = false;
            if (host_.refs_.exit_ref != LUA_NOREF) {
                lua_pushstring(host_.main_L_, "failure");
                LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
            }
            return NodeStatus::kFailure;
        }

        bool deactivate = false;
        auto ns = ParseReturnValues(result.values, deactivate);
        if (deactivate) {
            active_ = false;
            if (host_.refs_.exit_ref != LUA_NOREF) {
                auto* s = std::get_if<std::string>(&result.values[0]);
                lua_pushstring(host_.main_L_, s ? s->c_str() : "failure");
                LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
            }
        }
        return ns;
    }

    if (!active_) {
    active_ = true;
    if (host_.refs_.enter_ref != LUA_NOREF) {
        PushArgsTable(host_.main_L_, args_);
        LuaCallMethod(host_.main_L_, host_.refs_.enter_ref, host_.refs_.table_ref, 1);
    }
    }

    lua_State* co = host_.lua_context_->AcquireCoroutine();

    lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.tick_ref);
    lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.table_ref);

    host_.lua_context_->SetCoCompleteCallback(co, [this](ScriptResult r) {
        has_result_ = true;
        result_ = std::move(r);
    });

    int nresults = 0;
    int status = lua_resume(co, host_.main_L_, 1, &nresults);

    if (status == LUA_YIELD) {
        yielded_co_ = co;
        return NodeStatus::kRunning;
    }

    host_.lua_context_->RemoveCoCompleteCallback(co);

    if (status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        std::string msg = "'" + name_ + "' error: " + (err ? err : "unknown");
        spdlog::error("ScriptNode::Tick: {}", msg);
        set_last_error(std::move(msg));
        lua_pop(co, 1);
        active_ = false;
        if (host_.refs_.exit_ref != LUA_NOREF) {
            lua_pushstring(host_.main_L_, "failure");
            LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
        }
        return NodeStatus::kFailure;
    }

    auto values = LuaRuntime::PeekValues(co, nresults);
    lua_pop(co, nresults);

    bool deactivate = false;
    auto ns = ParseReturnValues(values, deactivate);
    if (deactivate) {
        active_ = false;
        if (host_.refs_.exit_ref != LUA_NOREF) {
            auto* s = std::get_if<std::string>(&values[0]);
            lua_pushstring(host_.main_L_, s ? s->c_str() : "failure");
            LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 1);
        }
    }
    return ns;
}

void ScriptNode::Reset() {
    if (yielded_co_ != nullptr) {
        host_.lua_context_->RemoveCoCompleteCallback(yielded_co_);
        yielded_co_ = nullptr;
        has_result_ = false;
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
        host_.lua_context_->RemoveCoCompleteCallback(yielded_co_);
        yielded_co_ = nullptr;
        has_result_ = false;
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
