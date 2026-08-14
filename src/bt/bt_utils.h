#pragma once

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <spdlog/spdlog.h>

#include "lua_runtime.h"
#include "lua_types.h"
#include "time_utils.h"

// --- Blackboard param references (`$key`) ---
//
// A Script node/condition `params` value that is a string starting with `$` is
// NOT a literal: at the node's Enter time the `$`-prefixed value is read from
// the blackboard and forwarded into Enter as that param's value. `$foo` reads
// blackboard key "foo". `$$foo` is the escape form — it yields the literal
// string "$foo" (a leading `$$` collapses to a single `$`). Only string VALUES
// are inspected, and only when the first character is `$`; any other string is
// passed through verbatim. Resolution happens at Enter (blackboard state is
// dynamic), not at parse/Init time.
struct BbParamRef {
    std::string key;  // blackboard key to read at Enter (the `$`-suffix)
};

// Classify a param string value. Returns:
//   - monostate      -> not a `$` value; caller keeps the original value.
//   - std::string    -> an escaped literal (leading `$$` collapsed to `$`);
//                       caller stores this literal string in place of the raw.
//   - BbParamRef     -> a blackboard reference to resolve at Enter.
//
// Rules (examined only when the first char is `$`):
//   "$foo"  -> BbParamRef{"foo"}
//   "$$"    -> literal "$"        (degenerate escape)
//   "$$foo" -> literal "$foo"
//   "$"     -> literal "$"        (lone `$`, no key)
//   "abc"   -> monostate (untouched)
inline std::variant<std::monostate, std::string, BbParamRef>
ResolveBbParamMarker(const std::string& s) {
    if (s.empty() || s[0] != '$') return std::monostate{};
    if (s.size() >= 2 && s[1] == '$') {
        // Escape: drop the leading `$$`, prepend one literal `$`.
        return std::string{"$" + s.substr(2)};
    }
    std::string key = s.substr(1);
    if (key.empty()) return std::string{"$"};  // lone `$` -> literal
    return BbParamRef{std::move(key)};
}

inline void ReleaseLuaRef(lua_State* L, int& ref) {
    if (ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    }
}

inline void LuaCallMethod(lua_State* L, int fn_ref, int self_ref, int extra_args) {
    int base = lua_gettop(L) - extra_args + 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, fn_ref);
    lua_insert(L, base);
    lua_rawgeti(L, LUA_REGISTRYINDEX, self_ref);
    lua_insert(L, base + 1);
    if (lua_pcall(L, 1 + extra_args, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        spdlog::error("LuaCallMethod: error: {}", err ? err : "unknown");
        lua_pop(L, 1);
    }
}

inline void PushArgsTable(lua_State* L,
                          const std::unordered_map<std::string, LuaValue>& args) {
    lua_newtable(L);
    for (const auto& [key, value] : args) {
        lua_pushstring(L, key.c_str());
        LuaRuntime::PushValues(L, {value});
        lua_settable(L, -3);
    }
}

// Format a script error for surfacing to callers (bt.exec's second return
// value). Lua 5.4 runtime errors already embed "source:line: " in the message,
// so it is returned verbatim when present; otherwise the debug source/line
// (stripped of Lua's "@" chunk marker) are synthesized so the result always
// carries a location. Returns "unknown error" only when nothing is available.
inline std::string FormatScriptError(const ScriptErrorDetail& d) {
    if (!d.message.empty()) return d.message;
    std::string src = d.source;
    if (!src.empty() && src.front() == '@') src.erase(0, 1);
    std::string out = src;
    if (d.line > 0) {
        if (!out.empty()) out += ":";
        out += std::to_string(d.line);
    }
    return out.empty() ? "unknown error" : out;
}

class ShuffledIndexTracker {
public:
    void EnsureShuffled(size_t count) {
        if (shuffled_) return;
        order_.resize(count);
        for (size_t i = 0; i < order_.size(); ++i) {
            order_[i] = i;
        }
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(order_.begin(), order_.end(), g);
        shuffled_ = true;
    }

    void Reset() {
        shuffled_ = false;
        order_.clear();
    }

    const std::vector<size_t>& order() const { return order_; }
    bool shuffled() const { return shuffled_; }

private:
    std::vector<size_t> order_;
    bool shuffled_ = false;
};

// Uniform random integer in the inclusive [lo, hi] range, drawn from the
// supplied URNG. Collapses to `lo` when the range is degenerate (hi <= lo),
// so lo==hi acts as a fixed value. Used by Pipeline to resolve a [lo,hi]
// $timeout/$retry range into a single value per step, per run.
template <typename URBG>
inline int RollIntInRange(int lo, int hi, URBG&& g) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(std::forward<URBG>(g));
}
