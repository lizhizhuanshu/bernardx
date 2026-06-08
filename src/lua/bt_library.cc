#include "bt_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <filesystem>

#include <spdlog/spdlog.h>
#include <sol/sol.hpp>

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

static async_simple::coro::Lazy<void> LoadTreeAsync(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    BehaviorTreeEngine::Ptr engine,
    std::string json_str,
    std::shared_ptr<ResourceProvider> tree_provider,
    std::string tree_relative_path,
    std::string project_path,
    std::shared_ptr<CodeProvider> code_provider,
    uint64_t expected_generation) {
    try {
        if (json_str.empty() && tree_provider) {
            json_str = co_await TreeParser::LoadTree(tree_provider.get(), tree_relative_path);
            if (engine->generation() != expected_generation) co_return;
            if (json_str.empty()) {
                rt->PushResume(handle, {false, std::string("failed to load tree")});
                co_return;
            }
        }

        auto [loaded, load_err] = engine->Load(json_str);
        if (engine->generation() != expected_generation) co_return;
        if (!loaded) {
            rt->PushResume(handle, {false, std::string(load_err.empty() ? "failed to parse JSON" : load_err)});
            co_return;
        }

        engine->SetProjectPath(project_path);

        if (code_provider && code_provider != rt->shared_code_provider()) {
            rt->set_shared_code_provider(code_provider);
        }

        auto init_error = co_await engine->InitScriptNodesAsync(rt->main_state(), rt.get());
        if (engine->generation() != expected_generation) co_return;
        if (!init_error.empty()) {
            rt->PushResume(handle, {false, std::string(init_error)});
            co_return;
        }

        auto sensor_error = co_await engine->InitSensorsAsync(rt->main_state(), rt.get());
        if (engine->generation() != expected_generation) co_return;
        if (!sensor_error.empty()) {
            rt->PushResume(handle, {false, std::string(sensor_error)});
            co_return;
        }

        engine->ActivateInitialSensors();
        rt->PushResume(handle, {true});
    } catch (const std::exception& e) {
        rt->PushResume(handle, {false, std::string(e.what())});
    }
}

static async_simple::coro::Lazy<void> TickTreeAsync(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    BehaviorTreeEngine::Ptr engine) {
    auto status = engine->TickOnce();
    rt->PushResume(handle, {std::string(NodeStatusToString(status))});
    co_return;
}

int bt_load(lua_State* L) {
    auto* engine = GetEngine(L);
    auto* lib = GetLibrary(L);
    const char* input = luaL_checkstring(L, 1);

    // Stop any previous tree (bumps generation, invalidating stale async ops)
    engine->Stop();

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no LuaRuntime");
        return 2;
    }
    if (!rt_ctx->executor()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "executor required");
        return 2;
    }

    auto handle = rt_ctx->PreYield(L);

    const auto& pp = lib->project_path();
    bool remote = !pp.empty() && pp[0] == '@';

    std::string json_str;
    std::shared_ptr<ResourceProvider> tree_provider;
    std::string tree_relative_path;
    std::string project_path = pp;
    std::shared_ptr<CodeProvider> code_provider;

    if (input[0] != '\0' && (input[0] == '{' || input[0] == '[')) {
        json_str = input;
    } else if (remote) {
        auto remote_base = pp.substr(1);
        tree_relative_path = remote_base.empty()
            ? std::string(input)
            : (remote_base + "/" + input);
        tree_provider = lib->resource_provider();

        std::vector<std::string> search_paths = {
            remote_base + "/scripts",
            remote_base + "/sensors",
            remote_base,
        };
        code_provider = std::make_shared<ResourceCodeProvider>(tree_provider, std::move(search_paths));
    } else {
        std::filesystem::path tree_path(input);
        if (!pp.empty()) {
            tree_path = std::filesystem::path(pp) / input;
        }
        tree_provider = std::make_shared<FileSystemResourceProvider>(tree_path.string());
    }

    if (!code_provider) {
        if (!pp.empty() && !remote) {
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
    }

    LoadTreeAsync(rt_ctx, handle, engine->shared_from_this(),
            std::move(json_str),
            std::move(tree_provider),
            std::move(tree_relative_path),
            std::move(project_path),
            std::move(code_provider),
            engine->generation())
        .via(rt_ctx->executor())
        .detach();

    return lua_yield(L, 0);
}

int bt_tick(lua_State* L) {
    auto* engine = GetEngine(L);

    if (!engine->IsLoaded()) {
        lua_pushnil(L);
        lua_pushstring(L, "no tree loaded");
        return 2;
    }

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) {
        lua_pushnil(L);
        lua_pushstring(L, "no LuaRuntime");
        return 2;
    }

    auto handle = rt_ctx->PreYield(L);

    TickTreeAsync(rt_ctx, handle, engine->shared_from_this())
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

int bt_set_project_path(lua_State* L) {
    auto* lib = GetLibrary(L);
    const char* path = luaL_checkstring(L, 1);
    lib->SetProjectPath(std::string(path));
    return 0;
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
        {"load", bt_load},
        {"tick", bt_tick},
        {"notify", bt_notify},
        {"get_status", bt_get_status},
        {"set_project_path", bt_set_project_path},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 2);
}

void BehaviorTreeLibrary::Close(lua_State* L) {
    engine_->Stop();
}
