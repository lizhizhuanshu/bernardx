#include "bt_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <filesystem>

#include <spdlog/spdlog.h>
#include <sol/sol.hpp>

#include "blackboard.h"
#include "file_system_code_provider.h"
#include "lua_runtime.h"
#include "tree_parser.h"
#include "types.h"

namespace {

LuaValue PopLuaValue(lua_State* L, int idx) {
    int t = lua_type(L, idx);
    if (t == LUA_TNIL) {
        return LuaValue(nullptr);
    } else if (t == LUA_TBOOLEAN) {
        return LuaValue(static_cast<bool>(lua_toboolean(L, idx)));
    } else if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, idx)) {
            return LuaValue(static_cast<int64_t>(lua_tointeger(L, idx)));
        }
        return LuaValue(lua_tonumber(L, idx));
    } else if (t == LUA_TSTRING) {
        size_t len;
        const char* s = lua_tolstring(L, idx, &len);
        return LuaValue(std::string(s, len));
    }
    return LuaValue(nullptr);
}

void PushLuaValue(lua_State* L, const LuaValue& v) {
    LuaRuntime::PushValues(L, {v});
}

BehaviorTreeEngine* GetEngine(lua_State* L) {
    return static_cast<BehaviorTreeEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

BehaviorTreeLibrary* GetLibrary(lua_State* L) {
    return static_cast<BehaviorTreeLibrary*>(lua_touserdata(L, lua_upvalueindex(2)));
}

// bt.run(json_or_path) — yields coroutine, resumes when BT tree completes
int bt_run(lua_State* L) {
    auto* engine = GetEngine(L);
    auto* lib = GetLibrary(L);
    const char* input = luaL_checkstring(L, 1);

    // Stop any previous run (resume its coroutine first)
    lib->StopBtThread(true);

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

    if (!engine->Load(json_str)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "failed to parse JSON");
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

    lib->pending_run_handle_ = handle;
    lib->pending_run_ctx_ = rt_ctx;

    lib->StartBtThread(code_provider, [rt_ctx, handle](const std::string& status) {
        std::vector<LuaValue> args;
        args.push_back(status);
        rt_ctx->PushResume(handle, std::move(args));
    });

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
    lib->StopBtThread(true);
    engine->Stop();
    return 0;
}

int bt_set(lua_State* L) {
    auto* engine = GetEngine(L);
    const char* key = luaL_checkstring(L, 1);
    auto value = PopLuaValue(L, 2);
    engine->blackboard().Set(key, std::move(value));
    return 0;
}

int bt_get(lua_State* L) {
    auto* engine = GetEngine(L);
    const char* key = luaL_checkstring(L, 1);
    auto value = engine->blackboard().Get(key);
    if (value.has_value()) {
        PushLuaValue(L, *value);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int bt_get_blackboard(lua_State* L) {
    GetEngine(L)->blackboard().PushAsTable(L);
    return 1;
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

int bt_get_current_node(lua_State* L) {
    auto node = GetEngine(L)->GetCurrentNode();
    lua_pushstring(L, node.c_str());
    return 1;
}

int bt_set_project_path(lua_State* L) {
    auto* lib = GetLibrary(L);
    const char* path = luaL_checkstring(L, 1);
    lib->SetProjectPath(std::string(path));
    return 0;
}

}  // namespace

BehaviorTreeLibrary::BehaviorTreeLibrary() {
    engine_ = std::make_shared<BehaviorTreeEngine>();
}

BehaviorTreeLibrary::~BehaviorTreeLibrary() {
    StopBtThread();
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
        {"set", bt_set},
        {"get", bt_get},
        {"get_blackboard", bt_get_blackboard},
        {"notify", bt_notify},
        {"get_status", bt_get_status},
        {"get_current_node", bt_get_current_node},
        {"set_project_path", bt_set_project_path},
        {nullptr, nullptr}
    };

    luaL_setfuncs(L, funcs, 2);
}

void BehaviorTreeLibrary::Close(lua_State* L) {
    StopBtThread();
    engine_->Stop();
}

void BehaviorTreeLibrary::StartBtThread(std::shared_ptr<CodeProvider> code_provider,
                                         CompletionCallback on_complete) {
    bt_context_ = LuaRuntime::Builder()
        .WithCodeProvider(std::move(code_provider))
        .Create();

    on_complete_ = std::move(on_complete);
    run_completed_.store(false);
    bt_running_.store(true);

    auto self = shared_from_this();
    bt_context_->executor()->schedule([self]() {
        if (!self->bt_context_) return;
        self->engine_->InitScriptNodes(
            self->bt_context_->main_state(), self->bt_context_.get());
        self->engine_->InitSensors(
            self->bt_context_->main_state(), self->bt_context_.get());
        self->engine_->ActivateInitialSensors();
        self->engine_->Run();
        self->ScheduleNextTick();
    });
}

void BehaviorTreeLibrary::StopBtThread(bool resume_pending) {
    bt_running_.store(false);
    engine_->DeactivateAllSensors();

    if (resume_pending && !run_completed_.load() && pending_run_ctx_) {
        std::vector<LuaValue> args;
        args.push_back(std::string("stopped"));
        pending_run_ctx_->PushResume(pending_run_handle_, std::move(args));
    }
    bt_context_.reset();

    run_completed_.store(false);
    pending_run_ctx_.reset();
    pending_run_handle_ = 0;
    on_complete_ = nullptr;
}

void BehaviorTreeLibrary::ScheduleNextTick() {
    if (!bt_running_.load(std::memory_order_acquire)) return;

    auto self = shared_from_this();
    bt_context_->executor()->schedule([self]() {
        if (!self->bt_running_.load(std::memory_order_acquire) || !self->bt_context_) return;

        auto status = self->engine_->TickOnce();

        if (status == NodeStatus::kSuccess || status == NodeStatus::kFailure) {
            self->engine_->Stop();
            self->run_completed_.store(true);
            if (self->on_complete_) {
                std::string status_str =
                    (status == NodeStatus::kSuccess) ? "success" : "failure";
                self->on_complete_(status_str);
            }
            return;
        }

        self->ScheduleNextTick();
    }, std::chrono::milliseconds(tick_interval_ms_));
}
