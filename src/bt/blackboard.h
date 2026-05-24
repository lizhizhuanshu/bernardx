#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "lua_runtime.h"

class Blackboard {
public:
    void Set(const std::string& key, LuaValue value) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = std::move(value);
    }

    std::optional<LuaValue> Get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = data_.find(key);
        if (it != data_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool Has(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.find(key) != data_.end();
    }

    void Remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.erase(key);
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.clear();
    }

    void PushAsTable(lua_State* L) const {
        std::unordered_map<std::string, LuaValue> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = data_;
        }
        lua_newtable(L);
        for (const auto& [key, value] : snapshot) {
            lua_pushstring(L, key.c_str());
            LuaRuntime::PushValues(L, {value});
            lua_settable(L, -3);
        }
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LuaValue> data_;
};
