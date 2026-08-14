#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "lua_runtime.h"

class Blackboard {
public:
    // A computed-value source for a key: every Get() of the key invokes the
    // provider and returns its result instead of a stored value. Installed
    // from Lua via blackboard.set_provider(key, fn); see blackboard_library.cc.
    // A key holds EITHER a static value OR a provider — the last writer wins
    // (Set replaces a provider, SetProvider replaces a value).
    using ValueProvider = std::function<LuaValue()>;

    void Set(const std::string& key, LuaValue value) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = Entry{std::move(value), nullptr};
    }

    void SetProvider(const std::string& key, ValueProvider provider) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = Entry{std::nullopt, std::move(provider)};
    }

    // Returns the key's value: a static entry returns the stored value; a
    // provider entry invokes the provider fresh on EVERY call (outside the
    // map lock, so a provider may itself touch the blackboard). A provider
    // returning nil yields a present nil LuaValue; a missing key yields
    // nullopt.
    std::optional<LuaValue> Get(const std::string& key) const {
        ValueProvider provider;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = data_.find(key);
            if (it == data_.end()) return std::nullopt;
            if (it->second.provider) {
                provider = it->second.provider;  // copy; invoke below, lock released
            } else {
                return it->second.value;
            }
        }
        return InvokeProvider(key, provider);
    }

    // True if the key holds a static value OR has a provider installed.
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
        std::unordered_map<std::string, Entry> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = data_;
        }
        lua_newtable(L);
        for (const auto& [key, entry] : snapshot) {
            LuaValue value = entry.provider ? InvokeProvider(key, entry.provider).value_or(LuaValue(nullptr))
                                            : entry.value.value_or(LuaValue(nullptr));
            lua_pushstring(L, key.c_str());
            LuaRuntime::PushValues(L, {value});
            lua_settable(L, -3);
        }
    }

private:
    // Invoke a provider with a recursion guard (a provider reading its own
    // key, or providers chaining too deeply, stops at the limit instead of
    // overflowing the stack). Runs with NO lock held.
    std::optional<LuaValue> InvokeProvider(const std::string& key,
                                           const ValueProvider& provider) const {
        thread_local int depth = 0;
        if (depth >= 8) {
            spdlog::error("Blackboard: provider recursion limit reached at key '{}'",
                          key);
            return std::nullopt;
        }
        ++depth;
        LuaValue v = provider();
        --depth;
        return v;
    }

    struct Entry {
        std::optional<LuaValue> value;  // static value (when no provider)
        ValueProvider provider;         // computed source (when installed)
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> data_;
};
