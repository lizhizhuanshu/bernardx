#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_context.h"

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

    // Non-copyable
    ActiveSensor(const ActiveSensor&) = delete;
    ActiveSensor& operator=(const ActiveSensor&) = delete;

    // Initialize: load sensor script and extract Enter/Tick/Exit refs (BT thread only)
    // base_path is the BT project root for resolving relative script paths
    void Init(lua_State* L, LuaContext* ctx, const std::string& base_path);

    // Activate: call Enter, mark active, run first Tick
    void Activate(Blackboard& bb);

    // Deactivate: cancel coroutine, call Exit(bb), mark inactive
    // bb is optional — if null, Exit is not called (used during destruction)
    void Deactivate(Blackboard* bb = nullptr);

    // Check if sensor interval has elapsed and not currently yielding
    bool TickReady(int64_t now_ms) const;

    // Execute sensor Tick in coroutine
    void RunOnce(Blackboard& bb);

    // Update next run time
    void ScheduleNext(int64_t now_ms);

    const std::string& name() const { return spec_.name; }
    int64_t interval_ms() const { return spec_.interval_ms; }
    bool is_active() const { return active_; }
    bool is_loaded() const { return tick_ref_ != LUA_NOREF; }

private:
    // Synchronous call via lua_pcall (for Enter/Exit — no yield)
    void CallSync(int fn_ref);

    // Parse return value from coroutine, write to blackboard
    void HandleResult(const std::vector<LuaValue>& values, Blackboard& bb);

    SensorSpec spec_;
    lua_State* main_L_ = nullptr;
    LuaContext* lua_context_ = nullptr;

    int enter_ref_ = LUA_NOREF;
    int tick_ref_ = LUA_NOREF;
    int exit_ref_ = LUA_NOREF;

    lua_State* yielded_co_ = nullptr;
    bool active_ = false;
    int64_t next_run_ms_ = 0;
};
