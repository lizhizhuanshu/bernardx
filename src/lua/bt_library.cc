#include "bt_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <chrono>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <sol/sol.hpp>

#include <async_simple/coro/Sleep.h>

#include "behavior_tree_engine.h"
#include "file_system_code_provider.h"
#include "file_system_resource_provider.h"
#include "lua_runtime.h"
#include "lua_value_utils.h"
#include "resource_code_provider.h"
#include "resource_provider.h"
#include "tree_parser.h"
#include "types.h"

namespace {

BehaviorTreeEngine* GetEngine(lua_State* L) {
    return static_cast<BehaviorTreeEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

BehaviorTreeLibrary* GetLibrary(lua_State* L) {
    return static_cast<BehaviorTreeLibrary*>(lua_touserdata(L, lua_upvalueindex(2)));
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
    std::string json_str,
    std::shared_ptr<ResourceProvider> tree_provider,
    std::string tree_relative_path,
    std::string project_path,
    std::shared_ptr<CodeProvider> code_provider,
    uint64_t expected_generation,
    int max_step,
    int64_t timeout_ms,
    int64_t interval_ms) {
    try {
        // 1. Load tree
        if (json_str.empty() && tree_provider) {
            json_str = co_await TreeParser::LoadTree(tree_provider.get(), tree_relative_path);
            if (engine->generation() != expected_generation) co_return;
            if (json_str.empty()) {
                rt->PushResume(handle, {nullptr, std::string("failed to load tree")});
                co_return;
            }
        }

        auto [loaded, load_err] = engine->Load(json_str);
        if (engine->generation() != expected_generation) co_return;
        if (!loaded) {
            rt->PushResume(handle, {nullptr, std::string(load_err.empty() ? "failed to parse JSON" : load_err)});
            co_return;
        }

        engine->SetProjectPath(project_path);

        if (code_provider && code_provider != rt->shared_code_provider()) {
            rt->set_shared_code_provider(code_provider);
        }

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
                rt->PushResume(handle, {std::string(NodeStatusToString(status))});
                co_return;
            }

            steps++;
            if (max_step > 0 && steps >= max_step) {
                engine->Stop();
                rt->PushResume(handle, {std::string("timeout")});
                co_return;
            }

            if (timeout_ms > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (elapsed >= timeout_ms) {
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
        rt->PushResume(handle, {nullptr, std::string(e.what())});
    }
}

int bt_run(lua_State* L) {
    auto* engine = GetEngine(L);
    auto* lib = GetLibrary(L);

    luaL_checktype(L, 1, LUA_TTABLE);

    // Extract fields from table
    std::string path;
    lua_getfield(L, 1, "path");
    if (lua_isstring(L, -1)) path = lua_tostring(L, -1);
    lua_pop(L, 1);

    std::string json;
    lua_getfield(L, 1, "json");
    if (lua_isstring(L, -1)) json = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (path.empty() && json.empty()) {
        lua_pushnil(L);
        lua_pushstring(L, "path or json required");
        return 2;
    }

    std::string project_path;
    lua_getfield(L, 1, "project_path");
    if (lua_isstring(L, -1)) project_path = lua_tostring(L, -1);
    lua_pop(L, 1);

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

    // Fall back to library's project_path if not provided in table
    if (project_path.empty()) {
        project_path = lib->project_path();
    }

    // Stop any previous tree
    engine->Stop();

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) {
        lua_pushnil(L);
        lua_pushstring(L, "no LuaRuntime");
        return 2;
    }
    if (!rt_ctx->executor()) {
        lua_pushnil(L);
        lua_pushstring(L, "executor required");
        return 2;
    }

    auto handle = rt_ctx->PreYield(L);

    const auto& pp = project_path;
    bool remote = !pp.empty() && pp[0] == '@';

    std::string json_str;
    std::shared_ptr<ResourceProvider> tree_provider;
    std::string tree_relative_path;
    std::string proj_path = pp;
    std::shared_ptr<CodeProvider> code_provider;
    std::string remote_base = remote ? pp.substr(1) : std::string{};

    if (!json.empty()) {
        json_str = json;
    } else if (remote) {
        tree_relative_path = remote_base.empty()
            ? std::string(path)
            : (remote_base + "/" + path);
        tree_provider = lib->resource_provider();
    } else {
        std::filesystem::path tree_path(path);
        if (!pp.empty()) {
            tree_path = std::filesystem::path(pp) / path;
        }
        tree_provider = std::make_shared<FileSystemResourceProvider>(tree_path.string());
    }

    if (remote && lib->resource_provider()) {
        std::vector<std::string> search_paths = {
            remote_base + "/scripts",
            remote_base + "/sensors",
            remote_base,
        };
        code_provider = std::make_shared<ResourceCodeProvider>(lib->resource_provider(), std::move(search_paths));
    } else if (!pp.empty() && !remote) {
        auto abs_pp = std::filesystem::absolute(pp);
        std::vector<std::string> search_paths = {
            (abs_pp / "scripts").string(),
            (abs_pp / "sensors").string(),
            abs_pp.string(),
        };
        if (!lib->main_libs_path().empty()) {
            search_paths.push_back(std::filesystem::absolute(lib->main_libs_path()).string());
        }
        code_provider = std::make_shared<FileSystemCodeProvider>(std::move(search_paths));
    } else {
        code_provider = rt_ctx->shared_code_provider();
    }

    RunTreeAsync(rt_ctx, handle, engine->shared_from_this(),
            std::move(json_str),
            std::move(tree_provider),
            std::move(tree_relative_path),
            std::move(proj_path),
            std::move(code_provider),
            engine->generation(),
            max_step, timeout_ms, interval_ms)
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

}  // namespace

BehaviorTreeLibrary::BehaviorTreeLibrary(std::shared_ptr<Blackboard> bb)
    : engine_(std::make_shared<BehaviorTreeEngine>(std::move(bb))) {}

BehaviorTreeLibrary::~BehaviorTreeLibrary() {
    engine_->Stop();
}

void BehaviorTreeLibrary::Open(lua_State* L) {
    lua_newtable(L);

    lua_pushlightuserdata(L, engine_.get());
    lua_pushlightuserdata(L, this);

    luaL_Reg funcs[] = {
        {"run", bt_run},
        {"notify", bt_notify},
        {"get_status", bt_get_status},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 2);
}

void BehaviorTreeLibrary::Close(lua_State* L) {
    engine_->Stop();
}
