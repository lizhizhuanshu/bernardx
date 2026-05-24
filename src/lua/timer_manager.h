#pragma once

#include <atomic>
#include <functional>
#include <unordered_map>

#include <async_simple/Executor.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_types.h"

class TimerManager {
public:
    using OnSleepExpired = std::function<void(AsyncHandle handle)>;
    using OnTimeoutExpired = std::function<void(int fn_ref)>;
    using OnTimeoutCancelled = std::function<void(int fn_ref)>;

    TimerManager(async_simple::Executor* executor,
                 OnSleepExpired on_sleep,
                 OnTimeoutExpired on_timeout,
                 OnTimeoutCancelled on_cancelled);

    void AddSleepTimer(int64_t deadline_ms, AsyncHandle handle);
    AsyncHandle AddTimeoutTimer(int64_t deadline_ms, int fn_ref);
    void CancelTimer(AsyncHandle handle);

    void CancelAll(lua_State* main_L);

    AsyncHandle NextHandle() { return next_handle_.fetch_add(1, std::memory_order_relaxed); }

    void set_executor(async_simple::Executor* executor) { executor_ = executor; }

private:
    enum class TimerType { kSleep, kSetTimeout };
    struct ActiveTimer { TimerType type; int fn_ref = LUA_NOREF; };

    async_simple::Executor* executor_;
    OnSleepExpired on_sleep_;
    OnTimeoutExpired on_timeout_;
    OnTimeoutCancelled on_cancelled_;
    std::atomic<AsyncHandle> next_handle_{1};
    std::unordered_map<AsyncHandle, ActiveTimer> active_timers_;
};
