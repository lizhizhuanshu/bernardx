#pragma once

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>

extern "C" {
#include "lua.h"
}

#include "lua_runtime.h"
#include "lua_types.h"

// Coroutine-based invocation of blackboard provider {get,set} functions.
//
// A provider is a Lua TABLE held by the blackboard as a registry LuaRef
// (installed via blackboard.set_provider). Both `get` and `set` may YIELD
// (sleep / await / a nested bb.get on another provider key): every call
// here runs the function on a pooled coroutine through
// LuaRuntime::CallWithCallback — the same pattern ScriptNode and
// ScriptCondition use for their Lua callbacks. The returned `yielded` flag
// and the completion callback follow CallWithCallback's contract:
//   false -> the call finished synchronously and on_complete has ALREADY
//            fired (no coroutine is outstanding);
//   true  -> the coroutine is suspended; on_complete fires later from the
//            runtime. The caller suspends (kRunning / PreYield+Yield) and
//            cancels via LuaRuntime::CancelCall(co) on abort/reset.
//
// `path` is the remaining dotted-key segments handed to the provider:
// get(path...) / set(value, path...).
namespace provider_call {

// Outcome of an invocation: `yielded` follows LuaRuntime::CallWithCallback
// — true means the coroutine is suspended and on_complete fires later from
// the runtime (the caller suspends / returns kRunning, and cancels via
// LuaRuntime::CancelCall(co) on abort); false means on_complete has
// ALREADY fired synchronously and nothing is outstanding.
struct InvokeOutcome {
    bool yielded = false;
    lua_State* co = nullptr;  // valid only when yielded
};

namespace detail {

// One shared thread-local depth budget for provider get/set calls: a
// provider whose get calls bb.get on ANOTHER provider key chains
// synchronously on the same thread until the first yield, and each link
// costs one depth. Capped at 8 so a cycle errors out instead of exhausting
// the C++ stack. The budget covers only the synchronous segment of an
// invocation (lua_resume until it returns control) — across a yield every
// resume starts a fresh stack, which cannot overflow.
inline int& GuardDepth() {
    thread_local int depth = 0;  // single entity: inline function local
    return depth;
}

// RAII: holds one depth slot while the invocation runs synchronously.
struct Guard {
    bool held = false;

    explicit Guard(const char* what) {
        if (GuardDepth() >= 8) {
            spdlog::error("blackboard provider {} recursion limit reached", what);
        } else {
            ++GuardDepth();
            held = true;
        }
    }
    ~Guard() {
        if (held) --GuardDepth();
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// Push the provider table + its `field` function onto a pooled coroutine;
// false (and the stack cleaned) when the field is absent.
inline bool PushProviderFn(lua_State* co, const LuaRef& provider,
                           const char* field) {
    lua_settop(co, 0);  // pooled coroutines may carry leftovers — clean slate
    lua_rawgeti(co, LUA_REGISTRYINDEX, provider->ref);  // shared registry
    lua_getfield(co, -1, field);
    lua_remove(co, -2);  // drop the table
    if (lua_isnil(co, -1)) {
        lua_pop(co, 1);
        return false;
    }
    return true;
}

inline void PushPath(lua_State* co, const std::vector<std::string>& path) {
    for (const auto& seg : path) {
        lua_pushlstring(co, seg.data(), seg.size());
    }
}

}  // namespace detail

// Launch provider.get(path...). on_complete ALWAYS receives LUA_OK with a
// single value (the get result; nil for an absent `get`, an empty return,
// or a runtime error — errors are logged here). See the header comment for
// the return-value contract.
inline InvokeOutcome InvokeProviderGet(
    LuaRuntime* rt, const LuaRef& provider, const std::vector<std::string>& path,
    std::function<void(ScriptResult)> on_complete) {
    lua_State* co = rt->AcquireCoroutine();
    if (!detail::PushProviderFn(co, provider, "get")) {
        rt->ReleaseCoroutine(co);
        ScriptResult r;
        r.status = LUA_OK;
        r.values = {LuaValue(nullptr)};
        on_complete(std::move(r));
        return {};
    }
    detail::PushPath(co, path);

    detail::Guard guard("get");
    if (!guard.held) {
        rt->ReleaseCoroutine(co);
        ScriptResult r;
        r.status = LUA_OK;
        r.values = {LuaValue(nullptr)};
        on_complete(std::move(r));
        return {};
    }
    bool yielded = rt->CallWithCallback(
        co, static_cast<int>(path.size()),
        [on_complete = std::move(on_complete)](ScriptResult r) mutable {
            if (r.status != LUA_OK) {
                spdlog::error("blackboard provider get error: {}", r.error);
            }
            ScriptResult ok;
            ok.status = LUA_OK;
            ok.values = {(r.status == LUA_OK && !r.values.empty())
                             ? std::move(r.values[0])
                             : LuaValue(nullptr)};
            on_complete(std::move(ok));
        });
    return {yielded, yielded ? co : nullptr};
}

// Launch provider.set(value, path...). on_complete receives LUA_OK once
// the write attempt is done (runtime errors inside `set` are logged here
// and still surface as LUA_OK — the write is attempted, its outcome is the
// provider's business), or LUA_ERRRUN "no set function" synchronously when
// the provider table has no `set` (callers warn and drop the write).
inline InvokeOutcome InvokeProviderSet(
    LuaRuntime* rt, const LuaRef& provider, const std::vector<std::string>& path,
    const LuaValue& value, std::function<void(ScriptResult)> on_complete) {
    // A LuaRef from another state cannot be pushed onto this coroutine
    // (its registry index would resolve to the wrong object) — refuse the
    // value instead of corrupting the registry.
    if (const auto* ref = std::get_if<LuaRef>(&value);
        ref && *ref && (*ref)->state() != rt->main_state()) {
        spdlog::warn(
            "blackboard provider set: value from another lua_State; dropped");
        ScriptResult r;
        r.status = LUA_OK;
        on_complete(std::move(r));
        return {};
    }
    lua_State* co = rt->AcquireCoroutine();
    if (!detail::PushProviderFn(co, provider, "set")) {
        rt->ReleaseCoroutine(co);
        ScriptResult r;
        r.status = LUA_ERRRUN;
        r.error = "no set function";
        on_complete(std::move(r));
        return {};
    }
    LuaRuntime::PushValues(co, {value});  // value FIRST, then the path
    detail::PushPath(co, path);

    detail::Guard guard("set");
    if (!guard.held) {
        rt->ReleaseCoroutine(co);
        ScriptResult r;
        r.status = LUA_OK;
        on_complete(std::move(r));
        return {};
    }
    bool yielded = rt->CallWithCallback(
        co, static_cast<int>(path.size()) + 1,
        [on_complete = std::move(on_complete)](ScriptResult r) mutable {
            if (r.status != LUA_OK) {
                spdlog::error("blackboard provider set error: {}", r.error);
            }
            ScriptResult ok;
            ok.status = LUA_OK;
            on_complete(std::move(ok));
        });
    return {yielded, yielded ? co : nullptr};
}

// One-shot synchronous attempt — ONLY for the bb.to_table snapshot: runs
// provider.get on a coroutine with a single lua_resume. A provider that
// yields is cancelled and reported as nil (async providers cannot be
// snapshotted; the error is logged).
inline std::optional<LuaValue> TryProviderGetSync(
    LuaRuntime* rt, const LuaRef& provider, const std::vector<std::string>& path) {
    lua_State* co = rt->AcquireCoroutine();
    if (!detail::PushProviderFn(co, provider, "get")) {
        rt->ReleaseCoroutine(co);
        return LuaValue(nullptr);
    }
    detail::PushPath(co, path);

    detail::Guard guard("get");
    if (!guard.held) {
        rt->ReleaseCoroutine(co);
        return LuaValue(nullptr);
    }
    int nresults = 0;
    int status = lua_resume(co, rt->main_state(),
                            static_cast<int>(path.size()), &nresults);
    if (status == LUA_YIELD) {
        spdlog::error(
            "blackboard provider get yielded (async not supported here); nil used");
        rt->CancelCall(co);  // clears the inner sleep + releases the coroutine
        return LuaValue(nullptr);
    }
    if (status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        spdlog::error("blackboard provider get error: {}",
                      err ? err : "unknown error");
        rt->ReleaseCoroutine(co);
        return LuaValue(nullptr);
    }
    LuaValue v = nresults > 0 ? LuaValueFromStack(co, -1) : LuaValue(nullptr);
    rt->ReleaseCoroutine(co);
    return v;
}

}  // namespace provider_call
