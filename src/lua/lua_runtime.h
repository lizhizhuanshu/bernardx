#pragma once

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <sol/sol.hpp>

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "code_provider.h"
#include "coroutine_pool.h"
#include "lua_extension.h"
#include "lua_library.h"
#include "lua_types.h"
#include "timer_manager.h"

namespace async_simple {
class Executor;
}  // namespace async_simple

// --- Value types (defined in lua_types.h) ---
// LuaRefBase, LuaRef, LuaValue

// --- Internal request types (used by LuaRuntime internals) ---

struct LoadScript {
    std::string chunk;
    std::string name;
};

struct CallRef {
    int fn_ref;
    std::vector<LuaValue> args;
    bool auto_unref = false;
};

using TaskKind = std::variant<LoadScript, CallRef>;

struct TaskRequest {
    TaskKind kind;
    async_simple::Promise<ScriptResult> promise;
};

// --- LuaRuntime: async Lua runtime with coroutine scheduler ---

class LuaRuntime : public std::enable_shared_from_this<LuaRuntime> {
public:
    using Ptr = std::shared_ptr<LuaRuntime>;

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;
    LuaRuntime(LuaRuntime&&) = delete;
    LuaRuntime& operator=(LuaRuntime&&) = delete;

    ~LuaRuntime();

    // --- Script API ---

    async_simple::coro::Lazy<ScriptResult> RunScript(const std::string& script);
    async_simple::coro::Lazy<ScriptResult> RunFile(const std::string& filename);
    async_simple::coro::Lazy<ScriptResult> CallFunction(int fn_ref, std::vector<LuaValue> args = {});
    sol::state& lua() { return *lua_; }

    // --- Async coroutine API (executor thread only) ---

    // Call a function ref in a new coroutine. Awaits completion (handles yield).
    async_simple::coro::Lazy<ScriptResult> CallAsync(int fn_ref, std::vector<LuaValue> args = {});
    // Load and run a file in a new coroutine. Awaits completion (handles yield).
    async_simple::coro::Lazy<ScriptResult> DoFileAsync(const std::string& path);

    // --- Extraspace ---

    static void SetExtraspace(lua_State* L, LuaRuntime* rt);
    static Ptr FromLuaState(lua_State* L);

    // --- Thread-safe submission ---

    void PushResume(AsyncHandle handle, std::vector<LuaValue> args = {});
    void PushRelease(std::vector<int> refs);
    void CallLuaFunction(int fn_ref, std::vector<LuaValue> args);

    // --- LuaRef factory ---

    LuaRef CreateRef(int ref, int type);

    // --- BT support ---

    lua_State* AcquireCoroutine() { return AcquireCo(); }
    void ReleaseCoroutine(lua_State* co) { ReleaseCo(co); }
    void SetCoCompleteCallback(lua_State* co, std::function<void(ScriptResult)> cb);
    void RemoveCoCompleteCallback(lua_State* co);
    std::shared_ptr<CodeProvider> shared_code_provider() const { return code_provider_; }

    // --- Internal accessors (used by builtin helpers in .cc) ---

    CodeProvider* code_provider() const { return code_provider_.get(); }
    async_simple::Executor* executor() const { return executor_; }
    lua_State* main_state() const { return main_L_; }
    std::optional<lua_CFunction> find_c_module(const std::string& name) const;
    std::shared_ptr<LuaLibrary> find_library(const std::string& name) const;

    void PushTask(TaskRequest task);
    void PushRequireRun(AsyncHandle handle, std::string source, std::string module_name);
    void PushLoadFileRun(AsyncHandle handle, std::string source, std::string filename);

    void AddSleepTimer(int64_t deadline_ms, AsyncHandle handle) {
        timer_mgr_->AddSleepTimer(deadline_ms, handle);
    }
    AsyncHandle AddTimeoutTimer(int64_t deadline_ms, int fn_ref) {
        return timer_mgr_->AddTimeoutTimer(deadline_ms, fn_ref);
    }
    void CancelTimer(AsyncHandle handle) {
        timer_mgr_->CancelTimer(handle);
    }

    void Interrupt();

    AsyncHandle PreYield(lua_State* co);
    static int Yield(lua_State* L);

    [[nodiscard]] static std::vector<LuaValue> PeekValues(lua_State* L, int nresults);
    static void PushValues(lua_State* L, const std::vector<LuaValue>& values);

    // --- Builder ---

    class Builder {
        friend class LuaRuntime;
    public:
        Builder& WithCodeProvider(std::shared_ptr<CodeProvider> provider);
        Builder& WithExecutor(async_simple::Executor& executor);
        Builder& Register(const std::string& name, lua_CFunction openf);
        Builder& RegisterExtension(std::shared_ptr<LuaExtension> extension);
        Builder& RegisterLibrary(std::shared_ptr<LuaLibrary> library);
        Builder& InheritFrom(LuaRuntime* parent);
        Ptr Create();

    private:
        std::shared_ptr<CodeProvider> code_provider_;
        async_simple::Executor* executor_ = nullptr;
        std::unordered_map<std::string, lua_CFunction> c_modules_;
        std::vector<std::shared_ptr<LuaExtension>> extensions_;
        std::unordered_map<std::string, std::shared_ptr<LuaLibrary>> libraries_;
    };

private:
    LuaRuntime();

    using CoCompleteCallback = std::function<void(ScriptResult)>;
    struct ResumeResult { lua_State* co = nullptr; int status = 0; };
    struct PendingEntry { lua_State* co; };

    // Setup / Shutdown
    void Setup(lua_State* main_L);
    void Shutdown();
    void SetupBuiltins(lua_State* main_L);
    void SetupCustomRequire(lua_State* main_L);

    // Configuration setters (used by Builder)
    void SetCodeProvider(std::shared_ptr<CodeProvider> provider) { code_provider_ = std::move(provider); }
    void SetExecutor(async_simple::Executor* executor) { executor_ = executor; }
    void SetCModules(std::unordered_map<std::string, lua_CFunction> modules) { c_modules_ = std::move(modules); }
    void SetExtensions(std::vector<std::shared_ptr<LuaExtension>> extensions) { extensions_ = std::move(extensions); }
    void SetLibraries(std::unordered_map<std::string, std::shared_ptr<LuaLibrary>> libraries) { libraries_ = std::move(libraries); }

    // Task processing (executor thread only)
    void ProcessTask(TaskRequest task);
    void ProcessRequireRun(std::string source, std::string module_name, AsyncHandle caller_handle);
    void ProcessLoadFileRun(std::string source, std::string filename, AsyncHandle caller_handle);

    // Resume
    ResumeResult DoResume(AsyncHandle handle, std::vector<LuaValue> args);

    // Async coroutine helper (executor thread only)
    async_simple::coro::Lazy<ScriptResult> AwaitCoroutine(lua_State* co, int status, int nresults);

    // Coroutine pool
    [[nodiscard]] lua_State* AcquireCo() { return co_pool_->Acquire(); }
    void ReleaseCo(lua_State* co) { co_pool_->Release(co); }
    void MaybeRecycleCo(lua_State* co, int status, int nresults);

    // --- Members ---

    std::unique_ptr<sol::state> lua_;
    lua_State* main_L_ = nullptr;

    std::shared_ptr<CodeProvider> code_provider_;
    std::vector<std::shared_ptr<LuaExtension>> extensions_;
    async_simple::Executor* executor_ = nullptr;

    std::unique_ptr<async_simple::Executor> owned_executor_;
    std::unordered_map<std::string, lua_CFunction> c_modules_;
    std::unordered_map<std::string, std::shared_ptr<LuaLibrary>> libraries_;

    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> interrupted_{false};
    std::unordered_map<AsyncHandle, PendingEntry> pending_;

    std::unique_ptr<TimerManager> timer_mgr_;
    std::unique_ptr<CoroutinePool> co_pool_;

    std::unordered_map<lua_State*, async_simple::Promise<ScriptResult>> script_promises_;
    std::unordered_map<lua_State*, CoCompleteCallback> co_complete_callbacks_;
};
