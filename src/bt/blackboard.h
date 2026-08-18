#pragma once

extern "C" {
#include "lua.h"
}

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "lua_runtime.h"
#include "lua_value_utils.h"
#include "provider_call.h"

// A read through Blackboard::Lookup: either a resolved static value, a
// miss, or a PROVIDER-served access — in which case the caller drives the
// provider itself (see provider_call.h): `provider` is the {get,set}
// table's registry ref and `path` the remaining dotted-key segments
// ({} for a flat key).
struct BbReadResult {
    enum class Kind { kMissing, kValue, kProvider };
    Kind kind = Kind::kMissing;
    LuaValue value;                 // kValue
    LuaRef provider;                // kProvider
    std::vector<std::string> path;  // kProvider (segments after the root)
};

// A write through Blackboard::Store: stored synchronously (flat key,
// literal dotted entry, rawset into a static table — or a mid-path miss
// that warned and dropped), rejected, or deferred to a provider the caller
// must invoke (provider_call::InvokeProviderSet).
struct BbWriteResult {
    enum class Kind { kStored, kProvider, kRejected };
    Kind kind = Kind::kStored;
    LuaRef provider;                // kProvider
    std::vector<std::string> path;  // kProvider
};

class Blackboard {
public:
    // Install a provider table (the {get,set} Lua table as a registry
    // LuaRef) for `key`. A key holds EITHER a static value OR a provider —
    // the last writer wins (Set replaces a provider, SetProvider replaces
    // a value).
    void SetProvider(const std::string& key, LuaRef table_ref) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_[key] = Entry{std::nullopt, std::move(table_ref)};
    }

    // Full read routing. Flat keys: static value / provider / missing.
    // Dotted keys: a literal entry for the FULL key wins; otherwise the
    // root segment decides — a provider root reports kProvider with the
    // remaining segments (the provider owns the whole path); a static
    // table root descends via rawget right here (data-only, any depth,
    // nullopt on any miss).
    BbReadResult Lookup(const std::string& key) const {
        BbReadResult out;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = data_.find(key);
            if (it == data_.end()) {
                // No literal entry. A dotted key falls through to its root
                // segment; a flat key is just missing.
                size_t dot = key.find('.');
                if (dot == std::string::npos) return out;  // kMissing
                auto root = data_.find(key.substr(0, dot));
                if (root == data_.end()) return out;       // kMissing
                if (root->second.provider) {
                    out.kind = BbReadResult::Kind::kProvider;
                    out.provider = root->second.provider;  // copy; caller invokes
                    SplitPathInto(key, dot, out.path);
                    return out;
                }
                if (!root->second.value) return out;       // kMissing
                out.value = *root->second.value;           // descend below
            } else if (it->second.provider) {
                // Literal entry holds a provider: a flat read reports it
                // (a literal key shadows any descent).
                out.kind = BbReadResult::Kind::kProvider;
                out.provider = it->second.provider;
                return out;
            } else if (it->second.value) {
                out.kind = BbReadResult::Kind::kValue;
                out.value = *it->second.value;
                return out;
            } else {
                return out;  // literal entry with no value: missing
            }
        }
        // Static table root: descend (no lock held).
        size_t dot = key.find('.');
        auto descended = DescendFrom(key, dot, std::move(out.value));
        if (!descended) return out;  // kMissing
        out.kind = BbReadResult::Kind::kValue;
        out.value = std::move(*descended);
        return out;
    }

    // Static-only convenience read: the resolved value for a static
    // (non-provider) access, nullopt otherwise. Provider-served keys
    // return nullopt BY DESIGN — use Lookup and drive the provider.
    std::optional<LuaValue> Get(const std::string& key) const {
        auto r = Lookup(key);
        if (r.kind != BbReadResult::Kind::kValue) return std::nullopt;
        return std::move(r.value);
    }

    // Full write routing. Flat keys store directly (replacing a provider).
    // Dotted keys: a literal entry for the FULL key wins (mirroring the
    // read shadowing rule); otherwise the root decides — a provider root
    // reports kProvider (caller invokes set with its copy of the value);
    // a static table root rawsets into that table right here (no
    // auto-vivify: a mid-path miss warns and reports kRejected); a missing
    // or non-table root stores a plain literal key.
    BbWriteResult Store(const std::string& key, const LuaValue& value) {
        BbWriteResult out;
        size_t dot = key.find('.');
        if (dot == std::string::npos) {
            std::lock_guard<std::mutex> lock(mutex_);
            data_[key] = Entry{value, LuaRef{}};
            return out;  // kStored
        }
        LuaRef provider;                    // report -> caller invokes
        std::optional<LuaValue> table_root;  // table -> rawset below
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A literal entry for the full dotted key shadows routing.
            if (data_.find(key) != data_.end()) {
                data_[key] = Entry{value, LuaRef{}};
                return out;  // kStored
            }
            auto root = data_.find(key.substr(0, dot));
            bool routed = false;
            if (root != data_.end()) {
                if (root->second.provider) {
                    provider = root->second.provider;
                    routed = true;
                } else if (root->second.value) {
                    auto* root_ref = std::get_if<LuaRef>(&*root->second.value);
                    if (root_ref && (*root_ref)->type == LUA_TTABLE) {
                        table_root = root->second.value;
                        routed = true;
                    }
                }
            }
            if (!routed) {
                data_[key] = Entry{value, LuaRef{}};
                return out;  // kStored (plain literal, previous behavior)
            }
        }
        if (provider) {
            out.kind = BbWriteResult::Kind::kProvider;
            out.provider = std::move(provider);
            SplitPathInto(key, dot, out.path);
            return out;
        }
        if (RawSetInto(key, dot, std::move(*table_root), value)) {
            return out;  // kStored
        }
        out.kind = BbWriteResult::Kind::kRejected;  // mid-path miss (warned)
        return out;
    }

    // Convenience write: Store, ignoring the routing report (sync paths
    // only — provider-rooted writes are silently unattempted).
    void Set(const std::string& key, const LuaValue& value) {
        Store(key, value);
    }

    // True if the key holds a static value OR has a provider installed.
    // Literal keys only: Has("a.b") never consults the root "a" (use Get
    // and check for nullopt to probe a dotted path).
    bool Has(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.find(key) != data_.end();
    }

    // Removes a literal key (and its provider). Literal keys only: dotted
    // keys are never split — Remove("a.b") drops a literal "a.b" entry,
    // restoring descent into root "a" if one exists.
    void Remove(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.erase(key);
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.clear();
    }

    // Snapshot to a Lua table. Provider entries use a SYNCHRONOUS
    // provider-call attempt: a provider that yields is cancelled and
    // snapshotted as nil (async providers cannot be snapshotted — the
    // error is logged).
    void PushAsTable(lua_State* L) const {
        LuaRuntime* rt = LuaRuntime::FromLuaState(L).get();
        std::unordered_map<std::string, Entry> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = data_;
        }
        lua_newtable(L);
        for (const auto& [key, entry] : snapshot) {
            LuaValue value = (entry.provider && rt)
                                 ? provider_call::TryProviderGetSync(rt, entry.provider, {})
                                       .value_or(LuaValue(nullptr))
                                 : entry.value.value_or(LuaValue(nullptr));
            lua_pushstring(L, key.c_str());
            LuaRuntime::PushValues(L, {value});
            lua_settable(L, -3);
        }
    }

private:
    // Split the segments AFTER first_dot into `out`: ("a.b.c", 1) -> {"b","c"}.
    static void SplitPathInto(const std::string& key, size_t first_dot,
                              std::vector<std::string>& out) {
        size_t pos = first_dot;
        while (pos != std::string::npos) {
            size_t next = key.find('.', pos + 1);
            out.emplace_back(key.substr(pos + 1,
                                        next == std::string::npos
                                            ? std::string::npos
                                            : next - pos - 1));
            pos = next;
        }
    }

    // Dotted-key descent (NO lock held; only for STATIC table roots — a
    // provider root consumes the path itself, see Lookup): starting from
    // the root table value, index each remaining segment of `key` (from
    // `first_dot` on) via lua_rawget on the ref's own state — tables only,
    // no metamethods. nullopt on any miss. Walks the stack (no
    // intermediate refs); only the final value is re-referenced, and only
    // via the owning runtime — without one a non-scalar result degrades to
    // nil rather than leaking a registry ref.
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

    // Dotted-key write into a STATIC table root (NO lock held): walk the
    // segments after first_dot via rawget on the ref's own state — tables
    // only, no metamethods, NO auto-vivify (any mid-path miss warns and
    // drops the write) — then rawset the final segment. The write lands in
    // the very table Lua may hold, so it is Lua-side visible. Returns
    // false when the write was rejected.
    bool RawSetInto(const std::string& key, size_t first_dot, LuaValue root,
                    LuaValue value) const {
        auto* ref = std::get_if<LuaRef>(&root);
        if (!ref || (*ref)->type != LUA_TTABLE) {
            spdlog::warn(
                "Blackboard: dotted write to '{}' failed (root not a table); no-op",
                key);
            return false;
        }
        lua_State* L = (*ref)->state();
        if (!L) {
            spdlog::warn(
                "Blackboard: dotted write to '{}' failed (table has no state); no-op",
                key);
            return false;
        }
        if (!CanPushOn(L, value)) {
            spdlog::warn(
                "Blackboard: dotted write to '{}' failed (value from another lua_State); no-op",
                key);
            return false;
        }
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
                spdlog::warn(
                    "Blackboard: dotted write to '{}' failed (empty path segment); no-op",
                    key);
                return false;
            }
            if (!lua_istable(L, -1)) {  // current level not a table
                lua_pop(L, 1);
                spdlog::warn(
                    "Blackboard: dotted write to '{}' failed (mid-path not a table); no-op",
                    key);
                return false;
            }
            if (next == std::string::npos) {  // final segment: set here
                lua_pushlstring(L, seg.data(), seg.size());
                PushLuaValue(L, value);
                lua_rawset(L, -3);
                lua_pop(L, 1);  // drop the table
                return true;
            }
            lua_pushlstring(L, seg.data(), seg.size());
            lua_rawget(L, -2);
            lua_remove(L, -2);  // drop the parent table
            if (lua_isnil(L, -1)) {  // absent mid field — no auto-vivify
                lua_pop(L, 1);
                spdlog::warn(
                    "Blackboard: dotted write to '{}' failed ('{}' missing mid-path); no-op",
                    key, seg);
                return false;
            }
            pos = next;
        }
    }

    // A LuaValue is safely pushable onto L only if the LuaRef it holds (if
    // any) belongs to L's state — a registry index from another state would
    // silently resolve to the wrong object there.
    static bool CanPushOn(lua_State* L, const LuaValue& value) {
        const auto* ref = std::get_if<LuaRef>(&value);
        return !ref || (*ref)->state() == L;
    }

    struct Entry {
        std::optional<LuaValue> value;  // static value (when no provider)
        LuaRef provider;                // {get,set} table ref (when installed)
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> data_;
};
