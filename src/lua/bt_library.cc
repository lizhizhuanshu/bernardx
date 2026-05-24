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
#include "lua_runtime.h"
#include "lua_value_utils.h"
#include "tree_parser.h"
#include "types.h"

using ResultArgs = std::vector<LuaValue>;

ResultArgs MakeRunResult(const std::string& status, const std::string& error) {
    ResultArgs args;
    if (!error.empty()) {
        args.push_back(false);
        args.push_back(error);
    } else {
        args.push_back(true);
        args.push_back(status);
    }
    return args;
}

namespace {

BehaviorTreeEngine* GetEngine(lua_State* L) {
    return static_cast<BehaviorTreeEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

BehaviorTreeLibrary* GetLibrary(lua_State* L) {
    return static_cast<BehaviorTreeLibrary*>(lua_touserdata(L, lua_upvalueindex(2)));
}

void StopAndResumePending(BehaviorTreeEngine* engine, BehaviorTreeLibrary* lib) {
    engine->StopLoop();
    if (!lib->run_completed() && lib->pending_run_ctx()) {
        lib->pending_run_ctx()->PushResume(lib->pending_run_handle(),
            MakeRunResult("stopped", {}));
    }
    lib->set_run_completed(false);
    lib->clear_pending_run();
}
int bt_run(lua_State* L) {
    auto* engine = GetEngine(L);
    auto* lib = GetLibrary(L);
    const char* input = luaL_checkstring(L, 1);

    StopAndResumePending(engine, lib);

    // Detect JSON vs directory path
    std::string json_str;
    if (input[0] == '{' || input[0] == '[') {
        json_str = input;
    } else {
        std::filesystem::path tree_path(input);
        if (!lib->project_path().empty()) {
            tree_path = std::filesystem::path(lib->project_path()) / input;
        }
        json_str = TreeParser::LoadTreeFromDirectory(tree_path.string());
        if (json_str.empty()) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "failed to load tree from directory");
            return 2;
        }
    }

    auto [loaded, load_err] = engine->Load(json_str);
    if (!loaded) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, load_err.empty() ? "failed to parse JSON" : load_err.c_str());
        return 2;
    }

    engine->SetProjectPath(lib->project_path());

    auto rt_ctx = LuaRuntime::FromLuaState(L);
    if (!rt_ctx) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no LuaRuntime");
        return 2;
    }

    auto handle = rt_ctx->PreYield(L);

    std::shared_ptr<CodeProvider> code_provider;
    if (!lib->project_path().empty()) {
        auto pp = std::filesystem::absolute(lib->project_path());
        std::vector<std::string> search_paths = {
            (pp / "scripts").string(),
            (pp / "sensors").string(),
            pp.string(),
        };
        if (!lib->main_libs_path().empty()) {
            search_paths.push_back(std::filesystem::absolute(lib->main_libs_path()).string());
        }
        code_provider = std::make_shared<FileSystemCodeProvider>(std::move(search_paths));
    } else {
        code_provider = rt_ctx->shared_code_provider();
    }

    lib->set_pending_run_handle(handle);
    lib->set_pending_run_ctx(rt_ctx);
    lib->set_run_completed(false);

    engine->StartLoop(code_provider, lib->tick_interval_ms(),
        [rt_ctx, handle, lib](const std::string& status, const std::string& error) {
            lib->set_run_completed(true);
            rt_ctx->PushResume(handle, MakeRunResult(status, error));
        },
        rt_ctx.get());

    return lua_yield(L, 0);
}

int bt_pause(lua_State* L) {
    GetEngine(L)->Pause();
    return 0;
}

int bt_resume(lua_State* L) {
    GetEngine(L)->Resume();
    return 0;
}

int bt_stop(lua_State* L) {
    auto* engine = GetEngine(L);
    auto* lib = GetLibrary(L);

    StopAndResumePending(engine, lib);
    engine->Stop();
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
    engine_->StopLoop();
}

void BehaviorTreeLibrary::Open(lua_State* L) {
    lua_newtable(L);

    lua_pushlightuserdata(L, engine_.get());
    lua_pushlightuserdata(L, this);

    luaL_Reg funcs[] = {
        {"run", bt_run},
        {"pause", bt_pause},
        {"resume", bt_resume},
        {"stop", bt_stop},
        {"notify", bt_notify},
        {"get_status", bt_get_status},
        {"set_project_path", bt_set_project_path},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 2);
}

void BehaviorTreeLibrary::Close(lua_State* L) {
    engine_->StopLoop();
    engine_->Stop();
}
