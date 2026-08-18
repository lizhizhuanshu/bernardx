#pragma once

extern "C" {
#include "lua.h"
}

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "lua_runtime.h"
#include "lua_value_utils.h"

class Blackboard {
public:
    // A computed-value source for a key: every Get() of the key invokes the
    // provider and returns its result instead of a stored value. Installed
    // from Lua via blackboard.set_provider(key, fn); see blackboard_library.cc.
    // A key holds EITHER a static value OR a provider — the last writer wins
    // (Set replaces a provider, SetProvider replaces a value).
    //
    // The argument is the remaining path segments of a dotted read: a flat
    // Get("cfg") calls fn({}); a dotted Get("cfg.net.ip") on a provider root
    // calls fn({"net","ip"}) - the provider owns the whole path and its
    // return value is used directly (no further descent).
    using ValueProvider = std::function<LuaValue(const std::vector<std::string>&)>;

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
    //
    // Dotted keys: a key containing '.' that has no literal entry resolves
    // its root segment, then the remaining path — Get("proxy.ip") resolves
    // blackboard[proxy] then field ip of that table (any depth). A static
    // table root descends via rawget (data-only); a provider root instead
    // RECEIVES the remaining segments as arguments — Get("cfg.net.ip") with
    // a provider on "cfg" calls fn("net","ip") and uses its return value
    // directly. A literal key always wins over descent ("proxy.ip" set
    // directly shadows proxy = {ip=..}). Any miss (root missing, a non-table
    // mid-path, an absent field) yields nullopt.
    std::optional<LuaValue> Get(const std::string& key) const {
        ValueProvider provider;  // non-empty -> invoke below, lock released
        std::optional<LuaValue> dotted_root;  // dotted miss -> root's value
        bool dotted = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = data_.find(key);
            if (it == data_.end()) {
                // No literal entry. A dotted key falls through to descent
                // against its root segment; a flat key is just missing.
                size_t dot = key.find('.');
                if (dot == std::string::npos) return std::nullopt;
                auto root = data_.find(key.substr(0, dot));
                if (root == data_.end()) return std::nullopt;
                dotted = true;
                if (root->second.provider) {
                    provider = root->second.provider;
                } else {
                    dotted_root = root->second.value;
                }
            } else if (it->second.provider) {
                // Literal entry holds a provider: plain read (a literal key
                // shadows any descent). Copy + invoke below — NEVER inside
                // the lock (a provider may re-enter the blackboard).
                provider = it->second.provider;
            } else {
                return it->second.value;
            }
        }
        if (!dotted) {
            // Flat provider read: no path to hand over.
            return InvokeProvider(key, provider, {});
        }
        size_t dot = key.find('.');
        // Dotted path: a provider root owns the whole path - hand it the
        // remaining segments and use its return value directly; a static
        // table root descends instead.
        if (provider) {
            return InvokeProvider(key, provider, SplitPath(key, dot));
        }
        return DescendFrom(key, dot, std::move(*dotted_root));
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
            LuaValue value = entry.provider ? InvokeProvider(key, entry.provider, {}).value_or(LuaValue(nullptr))
                                            : entry.value.value_or(LuaValue(nullptr));
            lua_pushstring(L, key.c_str());
            LuaRuntime::PushValues(L, {value});
            lua_settable(L, -3);
        }
    }

private:
    // Split the segments AFTER first_dot: SplitPath("a.b.c", 1) -> {"b","c"}.
    static std::vector<std::string> SplitPath(const std::string& key,
                                              size_t first_dot) {
        std::vector<std::string> segs;
        size_t pos = first_dot;
        while (pos != std::string::npos) {
            size_t next = key.find('.', pos + 1);
            segs.emplace_back(key.substr(pos + 1,
                                         next == std::string::npos
                                             ? std::string::npos
                                             : next - pos - 1));
            pos = next;
        }
        return segs;
    }

    // Dotted-key descent (NO lock held; only for STATIC table roots  — a
    // provider root consumes the path itself, see Get): starting from the
    // root table value, index each
    // remaining segment of `key` (from `first_dot` on) via lua_rawget on the
    // ref's own state — tables only, no metamethods. nullopt on any miss.
    // Walks the stack (no intermediate refs); only the final value is
    // re-referenced, and only via the owning runtime — without one a
    // non-scalar result degrades to nil rather than leaking a registry ref.
    std::optional<LuaValue> DescendFrom(const std::string& key, size_t first_dot,
                                        LuaValue root) const {
        auto* ref = std::get_if<LuaRef>(&root);
        if (!ref || (*ref)->type != LUA_TTABLE) return std::nullopt;
        lua_State* L = (*ref)->state();
        if (!L) return std::nullopt;

        lua_rawgeti(L, LUA_REGISTRYINDEX, (*ref)->ref);  // the root table
        size_t pos = first_dot;
        while (true) {
            size_t next = key.find('.', pos + 1);
            std::string seg = key.substr(pos + 1,
                                         next == std::string::npos
                                             ? std::string::npos
                                             : next - pos - 1);
            if (seg.empty()) {  // "a..b" / trailing dot
                lua_pop(L, 1);
                return std::nullopt;
            }
            if (!lua_istable(L, -1)) {  // mid-path non-table
                lua_pop(L, 1);
                return std::nullopt;
            }
            lua_pushlstring(L, seg.data(), seg.size());
            lua_rawget(L, -2);
            lua_remove(L, -2);  // drop the parent table
            if (lua_isnil(L, -1)) {  // absent (or nil-valued) field
                lua_pop(L, 1);
                return std::nullopt;
            }
            if (next == std::string::npos) break;  // final segment resolved
            pos = next;
        }
        LuaValue out = LuaValueFromStack(L, -1);  // adopts a fresh ref
        lua_pop(L, 1);
        return out;
    }

    // Invoke a provider with a recursion guard (a provider reading its own
    // key, or providers chaining too deeply, stops at the limit instead of
    // overflowing the stack). Runs with NO lock held.
    std::optional<LuaValue> InvokeProvider(
        const std::string& key, const ValueProvider& provider,
        const std::vector<std::string>& path) const {
        thread_local int depth = 0;
        if (depth >= 8) {
            spdlog::error("Blackboard: provider recursion limit reached at key '{}'",
                          key);
            return std::nullopt;
        }
        ++depth;
        LuaValue v = provider(path);
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
