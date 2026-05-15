#pragma once

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <async_simple/Promise.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "code_provider.h"
#include "lua_extension.h"
#include "lua_library.h"


namespace async_simple {
class Executor;
}

// --- Value types ---

using AsyncHandle = int64_t;

struct LuaRef {
    int ref;   // LUA_REGISTRYINDEX ref
    int type;  // lua_type value (LUA_TTABLE, LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD)
};

using LuaValue = std::variant<std::nullptr_t, bool, int64_t, double, std::string, LuaRef>;

struct ScriptResult {
    int status = LUA_ERRRUN;
    std::vector<LuaValue> values;
    std::string error;
};

// --- Request types ---

struct LoadScript {
    std::string chunk;
    std::string name;
};

struct CallRef {
    int fn_ref;
    std::vector<LuaValue> args;
    bool auto_unref = false;  // true for fire-and-forget callbacks (setTimeout)
};

using TaskKind = std::variant<LoadScript, CallRef>;

struct TaskRequest {
    TaskKind kind;
    async_simple::Promise<ScriptResult> promise;
};

struct ResumeRequest {
    AsyncHandle handle;
    std::vector<LuaValue> args;
};

using ReleaseRequest = std::vector<int>;  // LUA_REGISTRYINDEX refs to release

using WorkItem = std::variant<TaskRequest, ResumeRequest, ReleaseRequest>;  // int refs to release

// --- LuaContext: coroutine scheduler, builtins, and custom require ---

class LuaContext : public std::enable_shared_from_this<LuaContext> {
public:
    using Ptr = std::shared_ptr<LuaContext>;
    explicit LuaContext(lua_State* main_L);
    ~LuaContext();

    // --- Extraspace ---

    static void SetExtraspace(lua_State* L, LuaContext* ctx);
    static Ptr FromLuaState(lua_State* L);

    // --- Configuration (set by LuaRuntime during setup) ---

    void SetCodeProvider(std::shared_ptr<CodeProvider> provider) { code_provider_ = std::move(provider); }
    void SetExecutor(async_simple::Executor* executor) { executor_ = executor; }
    void SetCModules(std::unordered_map<std::string, lua_CFunction> modules) { c_modules_ = std::move(modules); }
    void SetExtensions(std::vector<std::shared_ptr<LuaExtension>> extensions) { extensions_ = std::move(extensions); }
    void SetLibraries(std::unordered_map<std::string, std::shared_ptr<LuaLibrary>> libraries) { libraries_ = std::move(libraries); }

    CodeProvider* code_provider() const { return code_provider_.get(); }
    async_simple::Executor* executor() const { return executor_; }
    std::optional<lua_CFunction> find_c_module(const std::string& name) const;
    std::shared_ptr<LuaLibrary> find_library(const std::string& name) const;
    lua_State* main_state() const { return main_L_; }

    // --- Setup (called on main thread during initialization) ---

    void Setup(lua_State* main_L);
    void Shutdown();

    // --- Thread-safe submission (called from any thread) ---

    void PushTask(TaskRequest task);
    void PushResume(AsyncHandle handle, std::vector<LuaValue> args = {});
    void PushRelease(std::vector<int> refs);
    void CallLuaFunction(int fn_ref, std::vector<LuaValue> args);

    // --- Event loop processing (event loop thread only) ---

    void ProcessExpiredTimers();
    bool DrainOneResume();
    bool DrainOneWork();
    bool DrainOneRelease();

    // --- Timer management ---

    void AddSleepTimer(int64_t deadline_ms, AsyncHandle handle);
    AsyncHandle AddTimeoutTimer(int64_t deadline_ms, int fn_ref);
    void CancelTimer(AsyncHandle handle);

    // --- Yield/Resume (event loop thread / any thread) ---

    AsyncHandle PreYield(lua_State* co);
    static int Yield(lua_State* L);

    // --- Coroutine completion callback (for ScriptNode yield/resume) ---

    using CoCompleteCallback = std::function<void(ScriptResult)>;
    void SetCoCompleteCallback(lua_State* co, CoCompleteCallback cb);
    void RemoveCoCompleteCallback(lua_State* co);

    // --- Wait/signal for event loop ---

    std::mutex& mutex() { return mutex_; }
    std::condition_variable& cv() { return cv_; }
    bool HasWork() const;
    std::optional<int64_t> NextTimerDeadline() const;

    // Shared access to code provider (for BT thread to share the same provider)
    std::shared_ptr<CodeProvider> shared_code_provider() const { return code_provider_; }

    // --- Value marshalling (stateless) ---

    static std::vector<LuaValue> PeekValues(lua_State* L, int nresults);
    static void PushValues(lua_State* L, const std::vector<LuaValue>& values);

    // --- Coroutine pool (event loop thread only) ---

    lua_State* AcquireCoroutine() { return AcquireCo(); }
    void ReleaseCoroutine(lua_State* co) { ReleaseCo(co); }

private:
    void SetupBuiltins(lua_State* main_L);
    void SetupCustomRequire(lua_State* main_L);

    struct PendingEntry {
        lua_State* co;
    };



    struct ResumeResult {
        lua_State* co = nullptr;
        int status = 0;
    };

    enum class TimerType { kSleep, kSetTimeout };

    struct TimerEntry {
        TimerType type;
        lua_State* co = nullptr;
        int fn_ref = LUA_NOREF;
        AsyncHandle handle = 0;
    };

    // Coroutine pool
    [[nodiscard]] lua_State* AcquireCo();
    void ReleaseCo(lua_State* co);
    void MaybeRecycleCo(lua_State* co, int status, int nresults);
    ResumeResult DoResume(AsyncHandle handle, std::vector<LuaValue> args);

    // Members
    lua_State* main_L_;
    std::shared_ptr<CodeProvider> code_provider_;
    std::vector<std::shared_ptr<LuaExtension>> extensions_;
    async_simple::Executor* executor_ = nullptr;
    std::unordered_map<std::string, lua_CFunction> c_modules_;
    std::unordered_map<std::string, std::shared_ptr<LuaLibrary>> libraries_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutting_down_ = false;
    std::queue<WorkItem> work_queue_;
    std::unordered_map<AsyncHandle, PendingEntry> pending_;
    std::multimap<int64_t, TimerEntry> timer_queue_;
    AsyncHandle next_handle_ = 1;

    // Active threads and their registry refs
    std::unordered_map<lua_State*, int> active_co_refs_;
    // Thread pool: idle threads with their registry refs
    std::vector<std::pair<lua_State*, int>> co_pool_;

    std::unordered_map<lua_State*, async_simple::Promise<ScriptResult>> script_promises_;

    // Coroutine completion callbacks (ScriptNode yield/resume tracking)
    std::unordered_map<lua_State*, CoCompleteCallback> co_complete_callbacks_;
};
