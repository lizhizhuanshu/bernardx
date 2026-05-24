#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "lua_types.h"

struct BtEvent {
    std::string name;
    LuaValue data;
};

class BtEventQueue {
public:
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
    mutable std::mutex mutex_;
    std::vector<BtEvent> queue_;
};
