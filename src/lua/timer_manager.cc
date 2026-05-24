#include "timer_manager.h"

#include <algorithm>
#include <chrono>

#include <lauxlib.h>
#include <lua.h>

#include <spdlog/spdlog.h>

#include "time_utils.h"

TimerManager::TimerManager(async_simple::Executor* executor,
                           OnSleepExpired on_sleep,
                           OnTimeoutExpired on_timeout,
                           OnTimeoutCancelled on_cancelled)
    : executor_(executor),
      on_sleep_(std::move(on_sleep)),
      on_timeout_(std::move(on_timeout)),
      on_cancelled_(std::move(on_cancelled)) {}

void TimerManager::AddSleepTimer(int64_t deadline_ms, AsyncHandle handle) {
    int64_t delay = std::max<int64_t>(0, deadline_ms - NowMs());
    active_timers_[handle] = ActiveTimer{TimerType::kSleep, LUA_NOREF};
    auto* self = this;
    executor_->schedule([self, handle]() {
        if (!self->active_timers_.erase(handle)) return;
        self->on_sleep_(handle);
    }, std::chrono::milliseconds(delay));
}

AsyncHandle TimerManager::AddTimeoutTimer(int64_t deadline_ms, int fn_ref) {
    auto handle = next_handle_.fetch_add(1, std::memory_order_relaxed);
    int64_t delay = std::max<int64_t>(0, deadline_ms - NowMs());
    active_timers_[handle] = ActiveTimer{TimerType::kSetTimeout, fn_ref};
    auto* self = this;
    executor_->schedule([self, handle, fn_ref]() {
        if (!self->active_timers_.erase(handle)) return;
        self->on_timeout_(fn_ref);
    }, std::chrono::milliseconds(delay));
    return handle;
}

void TimerManager::CancelTimer(AsyncHandle handle) {
    auto it = active_timers_.find(handle);
    if (it != active_timers_.end()) {
        if (it->second.fn_ref != LUA_NOREF) {
            on_cancelled_(it->second.fn_ref);
        }
        active_timers_.erase(it);
    }
}

void TimerManager::CancelAll(lua_State* main_L) {
    for (auto& [handle, timer] : active_timers_) {
        if (timer.fn_ref != LUA_NOREF) {
            luaL_unref(main_L, LUA_REGISTRYINDEX, timer.fn_ref);
        }
    }
    active_timers_.clear();
}
