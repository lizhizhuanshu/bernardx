#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "lua_types.h"

struct BtEvent {
    std::string name;
    LuaValue data;
};

// Per-tick context handed to every Node::Tick. Today it carries:
//   - the tick's CACHED TIME: the engine stamps the monotonic clock ONCE at
//     the start of each tick (BeginTick); nodes that need ms time (Wait,
//     Pipeline wait budgets) read now_ms() instead of calling the clock
//     themselves — every node in one tick sees the same instant, and
//     mid-tick clock reads disappear. Direct node users (tests) stamp it
//     the same way.
//   - the (vestigial) event queue of the removed bt.notify pipeline.
class BtEventQueue {
public:
    void BeginTick(int64_t now_ms) { tick_now_ms_ = now_ms; }
    int64_t now_ms() const { return tick_now_ms_; }

    void Push(BtEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(event));
    }

    std::vector<BtEvent> Drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<BtEvent> result;
        result.swap(queue_);
        return result;
    }

private:
    int64_t tick_now_ms_ = 0;
    mutable std::mutex mutex_;
    std::vector<BtEvent> queue_;
};
