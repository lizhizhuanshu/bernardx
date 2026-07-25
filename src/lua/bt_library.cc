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

static async_simple::coro::Lazy<void> RunTreeAsync(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    BehaviorTreeEngine::Ptr engine,
    std::unique_ptr<Node> root,
    uint64_t expected_generation,
    int max_step,
    int64_t timeout_ms,
    int64_t interval_ms,
    bool trace_paths) {
    auto mark_terminal = [&](NodeStatus s, const char* note) {
        if (engine->generation() == expected_generation) {
            engine->path_tracer().MarkCurrentTerminal(s, note);
        }
    };
    try {
        engine->SetRoot(std::move(root));
        engine->SetTracing(trace_paths);
        if (engine->generation() != expected_generation) co_return;

        auto init_error = co_await engine->InitScriptNodesAsync(rt->main_state(), rt.get());
        if (engine->generation() != expected_generation) co_return;
        if (!init_error.empty()) {
            rt->PushResume(handle, {nullptr, std::string(init_error)});
            co_return;
        }

        auto sensor_error = co_await engine->InitSensorsAsync(rt->main_state(), rt.get());
        if (engine->generation() != expected_generation) co_return;
        if (!sensor_error.empty()) {
            rt->PushResume(handle, {nullptr, std::string(sensor_error)});
            co_return;
        }

        engine->ActivateInitialSensors();

        // 2. Tick loop
        int steps = 0;
        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            if (engine->generation() != expected_generation) co_return;

            auto status = engine->TickOnce();

            if (status != NodeStatus::kRunning) {
                mark_terminal(status, nullptr);
                rt->PushResume(handle, {std::string(NodeStatusToString(status))});
                co_return;
            }

            steps++;
            if (max_step > 0 && steps >= max_step) {
                mark_terminal(NodeStatus::kRunning, "timeout");
                engine->Stop();
                rt->PushResume(handle, {std::string("timeout")});
                co_return;
            }

            if (timeout_ms > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= timeout_ms) {
                    mark_terminal(NodeStatus::kRunning, "timeout");
                    engine->Stop();
                    rt->PushResume(handle, {std::string("timeout")});
                    co_return;
                }
            }

            if (interval_ms > 0) {
                co_await async_simple::coro::sleep(
                    std::chrono::milliseconds(interval_ms));
            }
        }
    } catch (const std::exception& e) {
        mark_terminal(NodeStatus::kRunning, "exception");
        rt->PushResume(handle, {nullptr, std::string(e.what())});
    }
}

int bt_run(lua_State* L) {
    auto* engine = GetEngine(L);

    luaL_checktype(L, 1, LUA_TTABLE);

    // tree (required), subtrees / sensors (optional, default to empty tables).
    lua_getfield(L, 1, "tree");
    int tree_idx = lua_absindex(L, -1);
    if (!lua_istable(L, tree_idx)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "tree (table) required");
        return 2;
    }

    lua_getfield(L, 1, "subtrees");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    int subtrees_idx = lua_absindex(L, -1);

    lua_getfield(L, 1, "sensors");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    int sensors_idx = lua_absindex(L, -1);

    int max_step = -1;
    lua_getfield(L, 1, "max_step");
    if (lua_isinteger(L, -1)) max_step = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);

    int64_t timeout_ms = 0;
    lua_getfield(L, 1, "timeout");
    if (lua_isinteger(L, -1)) timeout_ms = lua_tointeger(L, -1);
    lua_pop(L, 1);

    int64_t interval_ms = 0;
    lua_getfield(L, 1, "interval");
    if (lua_isinteger(L, -1)) interval_ms = lua_tointeger(L, -1);
    lua_pop(L, 1);

    bool trace_paths = true;
    lua_getfield(L, 1, "trace_paths");
    if (lua_isboolean(L, -1)) trace_paths = lua_toboolean(L, -1);
    lua_pop(L, 1);

    // Stop any previous tree
    engine->Stop();

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) {
        lua_pop(L, 3);
        lua_pushnil(L);
        lua_pushstring(L, "no LuaRuntime");
        return 2;
    }
    if (!rt_ctx->executor()) {
        lua_pop(L, 3);
        lua_pushnil(L);
        lua_pushstring(L, "executor required");
        return 2;
    }

    // Parse tree from Lua tables on the stack (synchronous).
    auto parse_result = LuaTreeParser::Parse(L, tree_idx, subtrees_idx, sensors_idx);
    lua_pop(L, 3);

    if (!parse_result.root) {
        lua_pushnil(L);
        lua_pushstring(L, parse_result.error.empty() ? "failed to parse tree" : parse_result.error.c_str());
        return 2;
    }

    auto handle = rt_ctx->PreYield(L);

    RunTreeAsync(rt_ctx, handle, engine->shared_from_this(),
            std::move(parse_result.root),
            engine->generation(),
            max_step, timeout_ms, interval_ms,
            trace_paths)
        .via(rt_ctx->executor())
        .detach();

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
        {"run", bt_run},
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
