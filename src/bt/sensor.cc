#include "sensor.h"

#include <filesystem>
#include <spdlog/spdlog.h>

#include "blackboard.h"

ActiveSensor::ActiveSensor(SensorSpec spec)
    : spec_(std::move(spec)) {}

ActiveSensor::~ActiveSensor() {
    Deactivate();
}

async_simple::coro::Lazy<bool> ActiveSensor::Init(lua_State* L, LuaRuntime* ctx, const std::string& base_path) {
    if (!co_await host_.LoadScript(L, ctx, base_path, spec_.script_path, false)) {
        co_return false;
    }
    co_return true;
}

void ActiveSensor::HandleResult(const std::vector<LuaValue>& values, Blackboard& bb) {
    if (!values.empty()) {
        bb.Set(spec_.name, values[0]);
    }
}

void ActiveSensor::Activate(Blackboard& bb) {
    if (active_) return;
    active_ = true;
    next_run_ms_ = 0;

    if (host_.refs_.enter_ref != LUA_NOREF && host_.main_L_) {
        PushArgsTable(host_.main_L_, spec_.args);
        LuaCallMethod(host_.main_L_, host_.refs_.enter_ref, host_.refs_.table_ref, 1);
    }

    RunOnce(bb);
}

void ActiveSensor::Deactivate(Blackboard* bb) {
    if (!active_) return;
    active_ = false;

    if (yielded_co_ != nullptr && host_.lua_context_) {
        host_.lua_context_->RemoveCoCompleteCallback(yielded_co_);
        host_.lua_context_->ReleaseCoroutine(yielded_co_);
        yielded_co_ = nullptr;
    }

    if (host_.refs_.exit_ref != LUA_NOREF && host_.main_L_) {
        LuaCallMethod(host_.main_L_, host_.refs_.exit_ref, host_.refs_.table_ref, 0);
    }
}

bool ActiveSensor::TickReady(int64_t now_ms) const {
    return active_ && is_loaded() && yielded_co_ == nullptr && now_ms >= next_run_ms_;
}

void ActiveSensor::ScheduleNext(int64_t now_ms) {
    next_run_ms_ = now_ms + spec_.interval_ms;
}

void ActiveSensor::RunOnce(Blackboard& bb) {
    if (!is_loaded() || !host_.main_L_ || !host_.lua_context_) return;
    if (yielded_co_ != nullptr) return;

    lua_State* co = host_.lua_context_->AcquireCoroutine();

    lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.tick_ref);
    lua_rawgeti(co, LUA_REGISTRYINDEX, host_.refs_.table_ref);

    auto* self = this;
    host_.lua_context_->SetCoCompleteCallback(co, [self, &bb](ScriptResult r) {
        if (r.status == LUA_OK) {
            self->HandleResult(r.values, bb);
        }
        self->yielded_co_ = nullptr;
    });

    int nresults = 0;
    int status = lua_resume(co, host_.main_L_, 1, &nresults);

    if (status == LUA_YIELD) {
        yielded_co_ = co;
        return;
    }

    host_.lua_context_->RemoveCoCompleteCallback(co);

    if (status == LUA_OK) {
        auto values = LuaRuntime::PeekValues(co, nresults);
        lua_pop(co, nresults);
        HandleResult(values, bb);
    } else {
        const char* err = lua_tostring(co, -1);
        spdlog::error("ActiveSensor::RunOnce: '{}' error: {}",
                       spec_.name, err ? err : "unknown");
        lua_pop(co, 1);
    }

    host_.lua_context_->ReleaseCoroutine(co);
}
