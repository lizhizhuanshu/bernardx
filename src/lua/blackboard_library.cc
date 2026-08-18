#include "blackboard_library.h"

#include <memory>
#include <optional>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <spdlog/spdlog.h>

#include "blackboard.h"
#include "lua_value_utils.h"
#include "provider_call.h"

namespace {

Blackboard* GetBB(lua_State* L) {
    return static_cast<Blackboard*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int bb_set(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    auto value = PopLuaValue(L, 2);
    auto w = bb->Store(key, value);
    if (w.kind != BbWriteResult::Kind::kProvider) {
        return 0;  // stored (or rejected with a warning) — done
    }
    // Provider-rooted dotted write: run provider.set as a coroutine. It
    // usually completes synchronously (returns here directly); if it
    // yields, this C call suspends the calling coroutine and resumes (with
    // no values) once the set finishes.
    auto rt = LuaRuntime::FromLuaState(L);
    if (!rt) {
        return luaL_error(L, "blackboard provider requires a LuaRuntime");
    }
    // Launch the provider write; a synchronous completion returns here
    // directly, a yielded one suspends this coroutine until the provider
    // finishes (see bb_get for the resume-bridge ordering).
    auto handle_box = std::make_shared<AsyncHandle>(0);
    auto out = provider_call::InvokeProviderSet(
        rt.get(), w.provider, w.path, value,
        [rt, handle_box](ScriptResult r) {
            if (r.status != LUA_OK) {
                spdlog::warn("blackboard provider set: {}", r.error);
            }
            if (*handle_box != 0) {
                rt->PushResume(*handle_box, {});
            }
        });
    if (!out.yielded) {
        return 0;  // completed synchronously
    }
    *handle_box = rt->PreYield(L);  // arm the resume bridge, THEN yield
    return rt->Yield(L);
}

// bb.set_provider(key, {get = fn, set = fn2}): install a provider for
// `key`. The second argument MUST be a table; at least one of its `get` /
// `set` fields must be a function, the other may be omitted.
//
//   get(...)   fresh value at the dotted path: bb.get("cfg.net.ip") calls
//              get("net","ip"); a flat bb.get("cfg") calls get(). Invoked
//              on EVERY read (Lua side, $key param resolution at Enter,
//              Blackboard condition comparisons, Pipeline edge params).
//   set(v, ...) routed dotted WRITE: bb.set("cfg.net.ip", v) calls
//              set(v, "net", "ip").
//
// Both run as COROUTINES and may yield (sleep / await / a nested bb.get on
// another provider key). bb.get / bb.set suspend transparently while such
// a provider runs (a synchronous provider returns without suspending). A
// get error logs and reads as nil; a missing `get` reads as nil; a
// missing `set` warns and drops the write. The provider replaces any
// static value (and a flat bb.set replaces a provider); bb.remove /
// bb.clear drop it, releasing the table ref.
int bb_set_provider(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    if (lua_type(L, 2) != LUA_TTABLE) {
        return luaL_error(L,
                          "blackboard provider must be a table "
                          "{get = fn, set = fn} (got %s)",
                          luaL_typename(L, 2));
    }
    auto rt = LuaRuntime::FromLuaState(L);
    if (!rt) {
        return luaL_error(L, "blackboard provider requires a LuaRuntime");
    }

    // Validate the shape BEFORE taking the registry ref: luaL_error
    // longjmps, which would skip destruction of locals holding the ref.
    // Each field must be a function or nil/absent.
    bool has_get = false, has_set = false;
    for (const char* field : {"get", "set"}) {
        lua_getfield(L, 2, field);
        bool fn = lua_type(L, -1) == LUA_TFUNCTION;
        bool absent = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!fn && !absent) {
            return luaL_error(
                L, "blackboard provider field '%s' must be a function", field);
        }
        (field[0] == 'g' ? has_get : has_set) = fn;
    }
    if (!has_get && !has_set) {
        return luaL_error(L,
                          "blackboard provider table needs a 'get' or 'set' "
                          "function");
    }
    // Ref the whole table (get/set are read at invocation time).
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    bb->SetProvider(key, rt->CreateRef(ref, LUA_TTABLE));
    return 0;
}

int bb_get(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    auto look = bb->Lookup(key);
    if (look.kind == BbReadResult::Kind::kValue) {
        PushLuaValue(L, look.value);
        return 1;
    }
    if (look.kind == BbReadResult::Kind::kMissing) {
        lua_pushnil(L);
        return 1;
    }
    // Provider-served: run provider.get as a coroutine. It usually
    // completes synchronously (returns here directly); if it yields, this
    // C call suspends the calling coroutine and the value arrives as this
    // call's return when it resumes.
    auto rt = LuaRuntime::FromLuaState(L);
    if (!rt) {
        return luaL_error(L, "blackboard provider requires a LuaRuntime");
    }
    // Launch the provider read; a synchronous completion returns here
    // directly, a yielded one suspends this coroutine until the provider
    // finishes (the resume bridge is armed only on the yielded branch —
    // the callback cannot fire in between, single-threaded executor).
    auto result = std::make_shared<std::optional<LuaValue>>();
    auto handle_box = std::make_shared<AsyncHandle>(0);
    auto out = provider_call::InvokeProviderGet(
        rt.get(), look.provider, look.path,
        [rt, result, handle_box](ScriptResult r) {
            LuaValue v =
                (!r.values.empty()) ? std::move(r.values[0]) : LuaValue(nullptr);
            if (*handle_box != 0) {
                rt->PushResume(*handle_box, {std::move(v)});
            } else {
                *result = std::move(v);
            }
        });
    if (!out.yielded) {
        PushLuaValue(L, result->value_or(LuaValue(nullptr)));
        return 1;  // completed synchronously — no suspension
    }
    *handle_box = rt->PreYield(L);  // arm the resume bridge, THEN yield
    return rt->Yield(L);  // resumed with the value as this call's return
}

int bb_has(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    lua_pushboolean(L, bb->Has(key));
    return 1;
}

int bb_remove(lua_State* L) {
    auto* bb = GetBB(L);
    const char* key = luaL_checkstring(L, 1);
    bb->Remove(key);
    return 0;
}

int bb_clear(lua_State* L) {
    GetBB(L)->Clear();
    return 0;
}

// bb.to_table(): snapshot to a Lua table. Provider entries use a
// SYNCHRONOUS provider-call attempt — a provider that yields is cancelled
// and snapshotted as nil (async providers cannot be snapshotted; the
// error is logged).
int bb_to_table(lua_State* L) {
    GetBB(L)->PushAsTable(L);
    return 1;
}

}  // namespace

BlackboardLibrary::BlackboardLibrary(std::shared_ptr<Blackboard> bb)
    : blackboard_(std::move(bb)) {}

void BlackboardLibrary::Open(lua_State* L) {
    lua_newtable(L);

    lua_pushlightuserdata(L, blackboard_.get());

    luaL_Reg funcs[] = {
        {"set", bb_set},
        {"set_provider", bb_set_provider},
        {"get", bb_get},
        {"has", bb_has},
        {"remove", bb_remove},
        {"clear", bb_clear},
        {"to_table", bb_to_table},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 1);
}
