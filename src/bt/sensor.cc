#include "sensor.h"

#include <filesystem>
#include <spdlog/spdlog.h>

#include "blackboard.h"

ActiveSensor::ActiveSensor(SensorSpec spec)
    : spec_(std::move(spec)) {}

ActiveSensor::~ActiveSensor() {
    Deactivate();

    // Release Lua registry refs
    if (main_L_) {
        if (enter_ref_ != LUA_NOREF) luaL_unref(main_L_, LUA_REGISTRYINDEX, enter_ref_);
        if (tick_ref_ != LUA_NOREF) luaL_unref(main_L_, LUA_REGISTRYINDEX, tick_ref_);
        if (exit_ref_ != LUA_NOREF) luaL_unref(main_L_, LUA_REGISTRYINDEX, exit_ref_);
        enter_ref_ = LUA_NOREF;
        tick_ref_ = LUA_NOREF;
        exit_ref_ = LUA_NOREF;
    }
}

void ActiveSensor::Init(lua_State* L, LuaContext* ctx, const std::string& base_path) {
    main_L_ = L;
    lua_context_ = ctx;

    std::string full_path = spec_.script_path;
    if (!base_path.empty() && !std::filesystem::path(spec_.script_path).is_absolute()) {
        full_path = std::filesystem::absolute(base_path + "/" + spec_.script_path).string();
    }

    if (luaL_loadfile(L, full_path.c_str()) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("ActiveSensor::Init: failed to load '{}': {}",
                       spec_.script_path, err ? err : "unknown");
        lua_pop(L, 1);
        return;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("ActiveSensor::Init: failed to execute '{}': {}",
                       spec_.script_path, err ? err : "unknown");
        lua_pop(L, 1);
        return;
    }

    // Support both table format {Enter=, Tick=, Exit=} and plain function
    if (lua_isfunction(L, -1)) {
        tick_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
        spdlog::info("ActiveSensor::Init: loaded '{}' → sensor '{}' (function)",
                     spec_.script_path, spec_.name);
        return;
    }

    if (!lua_istable(L, -1)) {
        spdlog::error("ActiveSensor::Init: '{}' did not return a table or function",
                       spec_.script_path);
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

    luaL_unref(L, LUA_REGISTRYINDEX, table_ref);

    if (tick_ref_ == LUA_NOREF) {
        spdlog::error("ActiveSensor::Init: '{}' missing required 'Tick' function",
                       spec_.script_path);
    }

    spdlog::info("ActiveSensor::Init: loaded '{}' → sensor '{}' (Enter={}, Tick={}, Exit={})",
                 spec_.script_path, spec_.name,
                 enter_ref_ != LUA_NOREF, tick_ref_ != LUA_NOREF,
                 exit_ref_ != LUA_NOREF);
}

void ActiveSensor::CallSync(int fn_ref) {
    lua_rawgeti(main_L_, LUA_REGISTRYINDEX, fn_ref);
    if (lua_pcall(main_L_, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(main_L_, -1);
        spdlog::error("ActiveSensor::CallSync: error: {}", err ? err : "unknown");
        lua_pop(main_L_, 1);
    }
}

void ActiveSensor::HandleResult(const std::vector<LuaValue>& values, Blackboard& bb) {
    if (!values.empty()) {
        bb.Set(spec_.name, values[0]);
    }
}

void ActiveSensor::Activate(Blackboard& bb) {
    if (active_) return;
    active_ = true;
    next_run_ms_ = 0;  // run immediately

    // Call Enter (synchronous, no yield)
    if (enter_ref_ != LUA_NOREF && main_L_) {
        bb.PushAsTable(main_L_);
        lua_rawgeti(main_L_, LUA_REGISTRYINDEX, enter_ref_);
        lua_insert(main_L_, -2);
        if (lua_pcall(main_L_, 1, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(main_L_, -1);
            spdlog::error("ActiveSensor::Activate: Enter error: {}", err ? err : "unknown");
            lua_pop(main_L_, 1);
        }
    }

    RunOnce(bb);
}

void ActiveSensor::Deactivate(Blackboard* bb) {
    if (!active_) return;
    active_ = false;

    if (yielded_co_ != nullptr && lua_context_) {
        lua_context_->RemoveCoCompleteCallback(yielded_co_);
        lua_context_->ReleaseCoroutine(yielded_co_);
        yielded_co_ = nullptr;
    }

    // Call Exit with blackboard (synchronous, no yield)
    if (exit_ref_ != LUA_NOREF && main_L_ && bb) {
        bb->PushAsTable(main_L_);
        lua_rawgeti(main_L_, LUA_REGISTRYINDEX, exit_ref_);
        lua_insert(main_L_, -2);
        if (lua_pcall(main_L_, 1, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(main_L_, -1);
            spdlog::error("ActiveSensor::Deactivate: Exit error: {}", err ? err : "unknown");
            lua_pop(main_L_, 1);
        }
    }
}

bool ActiveSensor::TickReady(int64_t now_ms) const {
    return active_ && is_loaded() && yielded_co_ == nullptr && now_ms >= next_run_ms_;
}

void ActiveSensor::ScheduleNext(int64_t now_ms) {
    next_run_ms_ = now_ms + spec_.interval_ms;
}

void ActiveSensor::RunOnce(Blackboard& bb) {
    if (!is_loaded() || !main_L_ || !lua_context_) return;

    // Still waiting for previous coroutine
    if (yielded_co_ != nullptr) return;

    lua_State* co = lua_context_->AcquireCoroutine();

    // Push Tick function
    lua_rawgeti(co, LUA_REGISTRYINDEX, tick_ref_);

    // Push blackboard as table argument
    bb.PushAsTable(co);

    // Register completion callback
    auto* self = this;
    lua_context_->SetCoCompleteCallback(co, [self, &bb](ScriptResult r) {
        if (r.status == LUA_OK) {
            self->HandleResult(r.values, bb);
        }
        self->yielded_co_ = nullptr;
    });

    int nresults = 0;
    int status = lua_resume(co, main_L_, 1, &nresults);

    if (status == LUA_YIELD) {
        yielded_co_ = co;
        return;
    }

    // Completed immediately (no yield)
    lua_context_->RemoveCoCompleteCallback(co);

    if (status == LUA_OK) {
        auto values = LuaContext::PeekValues(co, nresults);
        lua_pop(co, nresults);
        HandleResult(values, bb);
    } else {
        const char* err = lua_tostring(co, -1);
        spdlog::error("ActiveSensor::RunOnce: '{}' error: {}",
                       spec_.name, err ? err : "unknown");
        lua_pop(co, 1);
    }

    lua_context_->ReleaseCoroutine(co);
}
