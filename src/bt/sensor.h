#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_runtime.h"

struct SensorSpec {
    std::string name;
    std::string script_path;
    int64_t interval_ms;
};

class Blackboard;

class ActiveSensor {
public:
    explicit ActiveSensor(SensorSpec spec);
    ~ActiveSensor();

    ActiveSensor(const ActiveSensor&) = delete;
    ActiveSensor& operator=(const ActiveSensor&) = delete;

    void Init(lua_State* L, LuaRuntime* ctx, const std::string& base_path);

    // Release Lua registry refs while the Lua state is still alive.
    void ReleaseRefs();

    void Activate(Blackboard& bb);
    void Deactivate(Blackboard* bb = nullptr);

    bool TickReady(int64_t now_ms) const;
    void RunOnce(Blackboard& bb);
    void ScheduleNext(int64_t now_ms);

    const std::string& name() const { return spec_.name; }
    int64_t interval_ms() const { return spec_.interval_ms; }
    bool is_active() const { return active_; }
    bool is_loaded() const { return tick_ref_ != LUA_NOREF; }

private:
    // Colon-method call: push fn, push self, extra_args → pcall(L, 1+extra_args, 0, 0)
    void CallMethod(lua_State* L, int fn_ref, int extra_args = 0);

    void HandleResult(const std::vector<LuaValue>& values, Blackboard& bb);

    SensorSpec spec_;
    lua_State* main_L_ = nullptr;
    LuaRuntime* lua_context_ = nullptr;

    int script_table_ref_ = LUA_NOREF;
    int enter_ref_ = LUA_NOREF;
    int tick_ref_ = LUA_NOREF;
    int exit_ref_ = LUA_NOREF;

    lua_State* yielded_co_ = nullptr;
    bool active_ = false;
    int64_t next_run_ms_ = 0;
};
