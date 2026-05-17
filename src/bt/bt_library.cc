#include "bt_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <chrono>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <sol/sol.hpp>

#include "blackboard.h"
#include "file_system_code_provider.h"
#include "lua_context.h"
#include "tree_parser.h"
#include "types.h"

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
    LuaContext::PushValues(L, {v});
}

BehaviorTreeEngine* GetEngine(lua_State* L) {
    return static_cast<BehaviorTreeEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}

BehaviorTreeLibrary* GetLibrary(lua_State* L) {
    return static_cast<BehaviorTreeLibrary*>(lua_touserdata(L, lua_upvalueindex(2)));
}

// bt.run(json_or_path) — yields coroutine, resumes when BT tree completes
// Accepts either a JSON string or a directory path containing root.json + subtree files
// If project_path is set, non-JSON paths are resolved relative to project_path
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
        // Resolve relative to project path if set
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

    // Set project path on engine for script/sensor path resolution
    engine->SetProjectPath(lib->project_path());

    // Get LuaRuntime's LuaContext for yield/resume
    auto rt_ctx = LuaContext::FromLuaState(L);
    if (!rt_ctx) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no LuaContext");
        return 2;
    }

    // Yield the current coroutine
    auto handle = rt_ctx->PreYield(L);

    // Create BT-specific code provider based on project path, or fall back to main
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

    // Store pending run info so StopBtThread can resume the coroutine if needed
    lib->pending_run_handle_ = handle;
    lib->pending_run_ctx_ = rt_ctx;

    // Start BT thread; on completion, resume the yielded coroutine
    lib->StartBtThread(code_provider, [rt_ctx, handle](const std::string& status) {
        std::vector<LuaValue> args;
        args.push_back(status);
        rt_ctx->PushResume(handle, std::move(args));
    });

    return lua_yield(L, 0);
}

// bt.pause()
int bt_pause(lua_State* L) {
    GetEngine(L)->Pause();
    return 0;
}

// bt.resume()
int bt_resume(lua_State* L) {
    GetEngine(L)->Resume();
    return 0;
}

// bt.stop()
int bt_stop(lua_State* L) {
    auto* engine = GetEngine(L);
    auto* lib = GetLibrary(L);
    lib->StopBtThread(true);
    engine->Stop();
    return 0;
}

// bt.set(key, value)
int bt_set(lua_State* L) {
    auto* engine = GetEngine(L);
    const char* key = luaL_checkstring(L, 1);
    auto value = PopLuaValue(L, 2);
    engine->blackboard().Set(key, std::move(value));
    return 0;
}

// bt.get(key) -> value
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

// bt.get_blackboard() -> table
int bt_get_blackboard(lua_State* L) {
    GetEngine(L)->blackboard().PushAsTable(L);
    return 1;
}

// bt.notify(event_name, data)
int bt_notify(lua_State* L) {
    auto* engine = GetEngine(L);
    const char* name = luaL_checkstring(L, 1);
    auto data = PopLuaValue(L, 2);
    engine->Notify(name, std::move(data));
    return 0;
}

// bt.get_status() -> string
int bt_get_status(lua_State* L) {
    auto status = GetEngine(L)->GetStatus();
    lua_pushstring(L, status.c_str());
    return 1;
}

// bt.get_current_node() -> string
int bt_get_current_node(lua_State* L) {
    auto node = GetEngine(L)->GetCurrentNode();
    lua_pushstring(L, node.c_str());
    return 1;
}

// bt.set_project_path(path)
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
    // Create BT's own Lua state and LuaContext
    bt_lua_ = std::make_unique<sol::state>();
    bt_lua_->open_libraries();

    bt_context_ = std::make_shared<LuaContext>(bt_lua_->lua_state());
    LuaContext::SetExtraspace(bt_lua_->lua_state(), bt_context_.get());

    if (code_provider) {
        bt_context_->SetCodeProvider(std::move(code_provider));
    }
    bt_context_->Setup(bt_lua_->lua_state());

    // Initialize script nodes using BT's LuaContext
    engine_->InitScriptNodes(bt_lua_->lua_state(), bt_context_.get());

    // Initialize sensors and activate initial set
    engine_->InitSensors(bt_lua_->lua_state(), bt_context_.get());
    engine_->ActivateInitialSensors();

    // Store completion callback and start engine
    on_complete_ = std::move(on_complete);
    run_completed_.store(false);
    engine_->Run();

    // Start BT event loop thread
    bt_running_.store(true);
    bt_thread_ = std::thread(&BehaviorTreeLibrary::BtEventLoop, this);
}

void BehaviorTreeLibrary::StopBtThread(bool resume_pending) {
    bt_running_.store(false);
    engine_->DeactivateAllSensors();
    if (bt_context_) {
        bt_context_->cv().notify_all();
    }
    if (bt_thread_.joinable()) {
        bt_thread_.join();
    }

    // If BT was running but tree didn't complete (stopped externally),
    // resume the pending coroutine with "stopped".
    // Only do this when called from bt.stop() (event loop still running),
    // not during shutdown (event loop already stopped, nobody will drain the resume).
    if (resume_pending && !run_completed_.load() && pending_run_ctx_) {
        std::vector<LuaValue> args;
        args.push_back(std::string("stopped"));
        pending_run_ctx_->PushResume(pending_run_handle_, std::move(args));
    }
    run_completed_.store(false);
    pending_run_ctx_.reset();
    pending_run_handle_ = 0;

    // Cleanup BT's LuaContext
    if (bt_context_) {
        bt_context_->Shutdown();
        bt_context_.reset();
    }
    bt_lua_.reset();
    on_complete_ = nullptr;
}

void BehaviorTreeLibrary::BtEventLoop() {
    spdlog::info("BT event loop started (interval={}ms)", tick_interval_ms_);
    int64_t next_tick = NowMs() + tick_interval_ms_;

    while (bt_running_.load(std::memory_order_acquire)) {
        // Process BT's LuaContext events (timers, resumes for script nodes)
        bt_context_->ProcessExpiredTimers();
        while (bt_context_->DrainOneWork()) {
        }

        // BT tick if interval elapsed
        int64_t now = NowMs();
        if (now >= next_tick) {
            auto status = engine_->TickOnce();

            if (status == NodeStatus::kSuccess || status == NodeStatus::kFailure) {
                // Tree completed — fire callback and stop
                spdlog::info("BT event loop: tree completed with status={}",
                             status == NodeStatus::kSuccess ? "success" : "failure");
                engine_->Stop();
                run_completed_.store(true);
                if (on_complete_) {
                    std::string status_str =
                        (status == NodeStatus::kSuccess) ? "success" : "failure";
                    on_complete_(status_str);
                }
                break;
            }

            next_tick = now + tick_interval_ms_;
        }

        // Wait for next event: tick deadline or timer deadline
        std::unique_lock<std::mutex> lock(bt_context_->mutex());
        auto timer_deadline = bt_context_->NextTimerDeadline();

        int64_t wait_deadline = next_tick;
        if (timer_deadline.has_value() && *timer_deadline < wait_deadline) {
            wait_deadline = *timer_deadline;
        }

        int64_t wait_ms = std::max<int64_t>(0, wait_deadline - NowMs());
        bt_context_->cv().wait_for(lock, std::chrono::milliseconds(wait_ms), [this] {
            return !bt_running_.load(std::memory_order_acquire) || bt_context_->HasWork();
        });
    }

    spdlog::info("BT event loop stopped");
}
