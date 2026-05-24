#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "bt_utils.h"
#include "lua_script_host.h"
#include "lua_runtime.h"

struct SensorSpec {
    using ArgsMap = std::unordered_map<std::string, LuaValue>;
    std::string name;
    std::string description;
    std::string script_path;
    int64_t interval_ms;
    ArgsMap args;
};

class Blackboard;

class ActiveSensor {
public:
    explicit ActiveSensor(SensorSpec spec);
    ~ActiveSensor();

    ActiveSensor(const ActiveSensor&) = delete;
    ActiveSensor& operator=(const ActiveSensor&) = delete;

    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx, const std::string& base_path);

    void Activate(Blackboard& bb);
    void Deactivate(Blackboard* bb = nullptr);

    bool TickReady(int64_t now_ms) const;
    void RunOnce(Blackboard& bb);
    void ScheduleNext(int64_t now_ms);

    const std::string& name() const { return spec_.name; }
    const std::string& description() const { return spec_.description; }
    bool is_active() const { return active_; }
    bool is_loaded() const { return host_.is_loaded(); }

private:
    void HandleResult(const std::vector<LuaValue>& values, Blackboard& bb);

    SensorSpec spec_;
    LuaScriptHost host_;

    lua_State* yielded_co_ = nullptr;
    bool active_ = false;
    int64_t next_run_ms_ = 0;
};
