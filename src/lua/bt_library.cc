#include "bt_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <chrono>

#include <spdlog/spdlog.h>

#include <async_simple/coro/Sleep.h>

#include "behavior_tree_engine.h"
#include "lua_runtime.h"
#include "lua_tree_parser.h"
#include "lua_value_utils.h"
#include "node.h"
#include "types.h"

namespace {

BehaviorTreeEngine* GetEngine(lua_State* L) {
    return static_cast<BehaviorTreeEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

const char* NodeStatusToString(NodeStatus s) {
    switch (s) {
        case NodeStatus::kSuccess: return "success";
        case NodeStatus::kFailure: return "failure";
        default: return "running";
    }
}

// --- ready: build + init scripts/sensors + activate (yields until done) ---
static async_simple::coro::Lazy<void> ReadyAsync(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    BehaviorTreeEngine::Ptr engine,
    std::unique_ptr<Node> root,
    bool trace_paths,
    uint64_t expected_generation) {
    std::string err;
    engine->SetRoot(std::move(root));
    engine->SetTracing(trace_paths);
    if (engine->generation() == expected_generation) {
        auto e1 = co_await engine->InitScriptNodesAsync(rt->main_state(), rt.get());
        if (engine->generation() == expected_generation) {
            if (e1.empty()) {
                auto e2 = co_await engine->InitSensorsAsync(rt->main_state(), rt.get());
                if (engine->generation() == expected_generation && e2.empty()) {
                    engine->ActivateInitialSensors();
                } else {
                    err = e2;
                }
            } else {
                err = e1;
            }
        }
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
            if (err.empty()) rt->PushResume(h, {std::string(status)});
            else rt->PushResume(h, {nullptr, std::string(err)});
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
                finish(NodeStatusToString(status), "", nullptr);
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

int bt_ready(lua_State* L) {
    auto* engine = GetEngine(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    auto st = engine->state();
    if (st == BehaviorTreeEngine::BtState::kRunning ||
        st == BehaviorTreeEngine::BtState::kPaused) {
        lua_pushnil(L);
        lua_pushstring(L, "tree is running; call stop first");
        return 2;
    }

    lua_getfield(L, 1, "tree");
    int tree_idx = lua_absindex(L, -1);
    if (!lua_istable(L, tree_idx)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "tree (table) required");
        return 2;
    }
    lua_getfield(L, 1, "subtrees");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_newtable(L); }
    int subtrees_idx = lua_absindex(L, -1);
    lua_getfield(L, 1, "sensors");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_newtable(L); }
    int sensors_idx = lua_absindex(L, -1);

    bool trace_paths = true;
    lua_getfield(L, 1, "trace_paths");
    if (lua_isboolean(L, -1)) trace_paths = lua_toboolean(L, -1);
    lua_pop(L, 1);

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) { lua_pop(L, 3); lua_pushnil(L); lua_pushstring(L, "no LuaRuntime"); return 2; }
    if (!rt_ctx->executor()) { lua_pop(L, 3); lua_pushnil(L); lua_pushstring(L, "executor required"); return 2; }

    auto parse_result = LuaTreeParser::Parse(L, tree_idx, subtrees_idx, sensors_idx);
    lua_pop(L, 3);
    if (!parse_result.root) {
        lua_pushnil(L);
        lua_pushstring(L, parse_result.error.empty() ? "failed to parse tree" : parse_result.error.c_str());
        return 2;
    }

    engine->Stop();
    engine->ClearOutcome();
    engine->clear_await_handle();

    auto handle = rt_ctx->PreYield(L);
    uint64_t gen = engine->generation();
    ReadyAsync(rt_ctx, handle, engine->shared_from_this(),
               std::move(parse_result.root), trace_paths, gen)
        .via(rt_ctx->executor())
        .detach();
    return lua_yield(L, 0);
}

int bt_exec(lua_State* L) {
    auto* engine = GetEngine(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    auto st = engine->state();
    if (st == BehaviorTreeEngine::BtState::kRunning) {
        lua_pushstring(L, "running");  // no-op: already running
        return 1;
    }
    if (st != BehaviorTreeEngine::BtState::kReady &&
        st != BehaviorTreeEngine::BtState::kPaused) {
        lua_pushnil(L);
        lua_pushstring(L, st == BehaviorTreeEngine::BtState::kIdle
                              ? "no tree; call ready first"
                              : "run finished; call ready to restart");
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

    engine->ClearOutcome();
    engine->clear_await_handle();
    engine->set_state(BehaviorTreeEngine::BtState::kRunning);
    uint64_t gen = engine->generation();
    RunLoop(rt_ctx, engine->shared_from_this(), gen, max_step, timeout_ms, interval_ms)
        .via(rt_ctx->executor())
        .detach();

    lua_pushstring(L, "running");
    return 1;  // non-blocking: caller may now pause/resume/dump/await
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

int bt_await(lua_State* L) {
    auto* engine = GetEngine(L);
    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) { lua_pushnil(L); lua_pushstring(L, "no LuaRuntime"); return 2; }

    if (engine->last_outcome().done) {
        const auto& o = engine->last_outcome();
        if (o.error.empty()) { lua_pushstring(L, o.status.c_str()); return 1; }
        lua_pushnil(L);
        lua_pushstring(L, o.error.c_str());
        return 2;
    }
    if (engine->has_await_handle()) {
        lua_pushnil(L);
        lua_pushstring(L, "already awaiting");
        return 2;
    }
    auto st = engine->state();
    if (st != BehaviorTreeEngine::BtState::kRunning &&
        st != BehaviorTreeEngine::BtState::kPaused) {
        lua_pushnil(L);
        lua_pushstring(L, "nothing running");
        return 2;
    }
    AsyncHandle h = rt_ctx->PreYield(L);
    engine->set_await_handle(h);
    return lua_yield(L, 0);
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
        {"ready", bt_ready},
        {"exec", bt_exec},
        {"goto_path", bt_goto_path},
        {"stop", bt_stop},
        {"await", bt_await},
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
