#include "bt_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <chrono>

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include <async_simple/coro/Sleep.h>

#include "behavior_tree_engine.h"
#include "json_lua.h"
#include "lua_runtime.h"
#include "lua_value_utils.h"
#include "node.h"
#include "resource_provider.h"
#include "tree_parser.h"
#include "types.h"

namespace {

BehaviorTreeEngine* GetEngine(lua_State* L) {
    return static_cast<BehaviorTreeEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Read a string field from table `tbl` ("" if missing/not a string).
std::string ReadStringOpt(lua_State* L, int tbl, const char* key) {
    std::string result;
    lua_getfield(L, tbl, key);
    if (lua_isstring(L, -1)) {
        size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        result.assign(s, len);
    }
    lua_pop(L, 1);
    return result;
}

// Read a boolean field from table `tbl` (`def` if missing/not a boolean).
bool ReadBoolOpt(lua_State* L, int tbl, const char* key, bool def) {
    bool result = def;
    lua_getfield(L, tbl, key);
    if (lua_isboolean(L, -1)) result = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return result;
}

const char* NodeStatusToString(NodeStatus s) {
    switch (s) {
        case NodeStatus::kSuccess: return "success";
        case NodeStatus::kFailure: return "failure";
        default: return "running";
    }
}

// --- init: load+parse JSON root, init scripts (+conditions), activate (yields) ---
static async_simple::coro::Lazy<void> InitAsync(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    BehaviorTreeEngine::Ptr engine,
    std::string root_path,
    nlohmann::json root_params,
    std::shared_ptr<ResourceProvider> provider,
    bool trace_paths,
    uint64_t expected_generation) {
    auto parse_result = co_await TreeParser::LoadAndParse(root_path, provider, std::move(root_params));
    if (engine->generation() != expected_generation) {
        // Stopped mid-load; unblock the awaiter so it doesn't hang.
        rt->PushResume(handle, {std::string("stopped")});
        co_return;
    }
    if (!parse_result.root) {
        engine->set_state(BehaviorTreeEngine::BtState::kIdle);
        rt->PushResume(handle, {nullptr, parse_result.error.empty()
                                         ? std::string("failed to parse tree")
                                         : std::move(parse_result.error)});
        co_return;
    }

    std::string err;
    engine->SetRoot(std::move(parse_result.root));
    engine->SetTracing(trace_paths);
    if (engine->generation() == expected_generation) {
        err = co_await engine->InitScriptNodesAsync(rt->main_state(), rt.get());
    }
    if (engine->generation() != expected_generation) {
        // Stopped mid-init; unblock the awaiter so it doesn't hang.
        rt->PushResume(handle, {std::string("stopped")});
        co_return;
    }
    if (err.empty()) {
        engine->set_state(BehaviorTreeEngine::BtState::kReady);
        rt->PushResume(handle, {std::string("ready")});
    } else {
        engine->set_state(BehaviorTreeEngine::BtState::kIdle);
        rt->PushResume(handle, {nullptr, std::string(err)});
    }
}

// --- exec: pausable background tick loop. Launched detached; returns to the
// caller immediately. Stops on terminal/timeout, observes rt->paused() (host
// pause) and engine->generation() (stop).
static async_simple::coro::Lazy<void> RunLoop(
    std::shared_ptr<LuaRuntime> rt,
    BehaviorTreeEngine::Ptr engine,
    uint64_t expected_generation,
    int max_step,
    int64_t timeout_ms,
    int64_t interval_ms) {
    auto finish = [&](const std::string& status, const std::string& err,
                      const char* trace_note) {
        if (engine->generation() != expected_generation) return;
        NodeStatus term = (status == "success") ? NodeStatus::kSuccess
                          : (status == "failure") ? NodeStatus::kFailure
                                                  : NodeStatus::kRunning;
        engine->path_tracer().MarkCurrentTerminal(term, trace_note);
        engine->SetOutcome(status, err);
        engine->set_state(status == "success" ? BehaviorTreeEngine::BtState::kSuccess
                          : status == "failure" ? BehaviorTreeEngine::BtState::kFailure
                                                : BehaviorTreeEngine::BtState::kIdle);
        if (engine->has_await_handle()) {
            AsyncHandle h = engine->await_handle();
            engine->clear_await_handle();
            // Always return (status, err): status is the run outcome
            // ("success"/"failure"/"timeout"); err carries the actionable
            // detail — for failures, the offending script's file:line:message.
            rt->PushResume(h, {std::string(status), std::string(err)});
        }
    };
    try {
        int steps = 0;
        auto start_time = std::chrono::steady_clock::now();
        while (true) {
            if (engine->generation() != expected_generation) co_return;  // stopped
            if (rt->paused()) {
                engine->set_state(BehaviorTreeEngine::BtState::kPaused);
                co_await async_simple::coro::sleep(
                    std::chrono::milliseconds(std::max<int64_t>(10, interval_ms / 2)));
                continue;
            }
            engine->set_state(BehaviorTreeEngine::BtState::kRunning);

            auto status = engine->TickOnce();
            if (status != NodeStatus::kRunning) {
                // On failure, surface the root's propagated last_error() —
                // typically the offending script's "file:line: message" — so
                // callers can see WHY the tree failed instead of just "failure".
                std::string err = (status == NodeStatus::kFailure)
                                      ? engine->last_error()
                                      : std::string();
                finish(NodeStatusToString(status), err, nullptr);
                co_return;
            }
            steps++;
            if (max_step > 0 && steps >= max_step) {
                finish("timeout", "", "timeout");
                engine->Stop();
                co_return;
            }
            if (timeout_ms > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= timeout_ms) {
                    finish("timeout", "", "timeout");
                    engine->Stop();
                    co_return;
                }
            }
            if (interval_ms > 0) {
                co_await async_simple::coro::sleep(
                    std::chrono::milliseconds(interval_ms));
            }
        }
    } catch (const std::exception& e) {
        finish("failure", e.what(), "exception");
    }
}

int bt_init(lua_State* L) {
    auto* engine = GetEngine(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    auto st = engine->state();
    if (st == BehaviorTreeEngine::BtState::kRunning ||
        st == BehaviorTreeEngine::BtState::kPaused) {
        lua_pushnil(L);
        lua_pushstring(L, "tree is running; call stop first");
        return 2;
    }

    // root: path to the root node JSON file ("res://rel" resource or absolute). Required.
    std::string root_path = ReadStringOpt(L, 1, "root");
    if (root_path.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, "root (string path) required");
        return 2;
    }
    bool trace_paths = ReadBoolOpt(L, 1, "trace_paths", true);

    // params: optional table of template values forwarded into the root JSON by
    // substituting {{key}} placeholders (same rules as Subtree param forwarding).
    nlohmann::json root_params = nlohmann::json::object();
    lua_getfield(L, 1, "params");
    if (lua_istable(L, -1)) {
        root_params = LuaToJson(L, -1);
    }
    lua_pop(L, 1);

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) { lua_pushnil(L); lua_pushstring(L, "no LuaRuntime"); return 2; }
    if (!rt_ctx->executor()) { lua_pushnil(L); lua_pushstring(L, "executor required"); return 2; }

    engine->Stop();
    engine->ClearOutcome();
    engine->clear_await_handle();

    auto handle = rt_ctx->PreYield(L);
    uint64_t gen = engine->generation();
    InitAsync(rt_ctx, handle, engine->shared_from_this(),
               std::move(root_path),
               std::move(root_params),
               rt_ctx->shared_resource_provider(), trace_paths, gen)
        .via(rt_ctx->executor())
        .detach();
    return lua_yield(L, 0);
}

int bt_exec(lua_State* L) {
    auto* engine = GetEngine(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    auto st = engine->state();
    if (st != BehaviorTreeEngine::BtState::kReady &&
        st != BehaviorTreeEngine::BtState::kPaused) {
        lua_pushnil(L);
        lua_pushstring(L, st == BehaviorTreeEngine::BtState::kIdle
                              ? "no tree; call init first"
                              : "run finished; call init to restart");
        return 2;
    }

    int64_t interval_ms = 50;
    lua_getfield(L, 1, "interval");
    if (lua_isinteger(L, -1)) interval_ms = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (interval_ms <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "interval must be > 0");
        return 2;
    }
    int max_step = -1;
    lua_getfield(L, 1, "max_step");
    if (lua_isinteger(L, -1)) max_step = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    int64_t timeout_ms = 0;
    lua_getfield(L, 1, "timeout");
    if (lua_isinteger(L, -1)) timeout_ms = lua_tointeger(L, -1);
    lua_pop(L, 1);

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) { lua_pushnil(L); lua_pushstring(L, "no LuaRuntime"); return 2; }

    // exec blocks (yields) until the run finishes; the RunLoop resumes us with
    // the status. pause/resume is host-level (network) and applies during.
    engine->ClearOutcome();
    engine->set_state(BehaviorTreeEngine::BtState::kRunning);
    AsyncHandle handle = rt_ctx->PreYield(L);
    engine->set_await_handle(handle);
    uint64_t gen = engine->generation();
    RunLoop(rt_ctx, engine->shared_from_this(), gen, max_step, timeout_ms, interval_ms)
        .via(rt_ctx->executor())
        .detach();
    return lua_yield(L, 0);
}

int bt_goto_path(lua_State* L) {
    auto* engine = GetEngine(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> names;
    lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, 1, i);
        const char* s = lua_tostring(L, -1);
        if (!s) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushstring(L, "path must be an array of strings");
            return 2;
        }
        names.emplace_back(s);
        lua_pop(L, 1);
    }
    auto err = engine->GotoPath(names);
    if (err.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, err->c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int bt_stop(lua_State* L) {
    auto* engine = GetEngine(L);
    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (engine->has_await_handle() && rt_ctx) {
        AsyncHandle h = engine->await_handle();
        engine->clear_await_handle();
        engine->SetOutcome("stopped", "");
        rt_ctx->PushResume(h, {std::string("stopped")});
    }
    engine->Stop();
    engine->set_state(BehaviorTreeEngine::BtState::kIdle);
    return 0;
}

int bt_notify(lua_State* L) {
    auto* engine = GetEngine(L);
    const char* name = luaL_checkstring(L, 1);
    auto data = PopLuaValue(L, 2);
    engine->Notify(name, std::move(data));
    return 0;
}

int bt_get_status(lua_State* L) {
    auto status = GetEngine(L)->GetStatus();
    lua_pushstring(L, status.c_str());
    return 1;
}

int bt_dump_paths(lua_State* L) {
    GetEngine(L)->path_tracer().BuildLuaTable(L);
    return 1;
}

int bt_path_report(lua_State* L) {
    lua_pushstring(L, GetEngine(L)->path_tracer().RenderReport().c_str());
    return 1;
}

}  // namespace

BehaviorTreeLibrary::BehaviorTreeLibrary(std::shared_ptr<Blackboard> bb)
    : engine_(std::make_shared<BehaviorTreeEngine>(std::move(bb))) {}

BehaviorTreeLibrary::~BehaviorTreeLibrary() {
    engine_->Stop();
}

void BehaviorTreeLibrary::Open(lua_State* L) {
    lua_newtable(L);

    lua_pushlightuserdata(L, engine_.get());

    luaL_Reg funcs[] = {
        {"init", bt_init},
        {"exec", bt_exec},
        {"goto_path", bt_goto_path},
        {"stop", bt_stop},
        {"notify", bt_notify},
        {"get_status", bt_get_status},
        {"dump_paths", bt_dump_paths},
        {"path_report", bt_path_report},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 1);
}

void BehaviorTreeLibrary::Close(lua_State* L) {
    engine_->Stop();
}
