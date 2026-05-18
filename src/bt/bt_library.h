#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include <async_simple/coro/Lazy.h>

#include "behavior_tree_engine.h"
#include "lua_runtime.h"
#include "lua_library.h"

class BehaviorTreeLibrary : public LuaLibrary,
                            public std::enable_shared_from_this<BehaviorTreeLibrary> {
public:
    BehaviorTreeLibrary();
    ~BehaviorTreeLibrary() override;

    BehaviorTreeLibrary(const BehaviorTreeLibrary&) = delete;
    BehaviorTreeLibrary& operator=(const BehaviorTreeLibrary&) = delete;
    BehaviorTreeLibrary(BehaviorTreeLibrary&&) = delete;
    BehaviorTreeLibrary& operator=(BehaviorTreeLibrary&&) = delete;

    std::string name() const override { return "bt"; }
    void Open(lua_State* L) override;
    void Close(lua_State* L) override;

    BehaviorTreeEngine::Ptr engine() const { return engine_; }

    void SetTickIntervalMs(int64_t ms) { tick_interval_ms_.store(ms, std::memory_order_relaxed); }
    int64_t tick_interval_ms() const { return tick_interval_ms_.load(std::memory_order_relaxed); }

    void SetMainLibsPath(std::string path) { main_libs_path_ = std::move(path); }
    const std::string& main_libs_path() const { return main_libs_path_; }
    void SetProjectPath(std::string path) { project_path_ = std::move(path); }
    const std::string& project_path() const { return project_path_; }

    using CompletionCallback = std::function<void(const std::string& status)>;

    void StartBtThread(std::shared_ptr<CodeProvider> code_provider,
                       CompletionCallback on_complete);

    // Stop BT thread. If resume_pending=true, resumes the yielded bt.run() coroutine.
    void StopBtThread(bool resume_pending = false);

    // Accessed by bt_run/bt_stop C functions (via upvalue pointer)
    AsyncHandle pending_run_handle_ = 0;
    LuaRuntime::Ptr pending_run_ctx_;
    std::atomic<bool> run_completed_{false};

private:
    async_simple::coro::Lazy<void> BtTickLoop(LuaRuntime::Ptr ctx,
                                               CompletionCallback on_complete);

    BehaviorTreeEngine::Ptr engine_;
    LuaRuntime::Ptr bt_context_;

    std::atomic<bool> bt_running_{false};
    std::atomic<int64_t> tick_interval_ms_{100};

    std::string project_path_;
    std::string main_libs_path_;

    // Synchronization: StopBtThread blocks until BtTickLoop exits.
    std::mutex tick_loop_mu_;
    std::condition_variable tick_loop_cv_;
    bool tick_loop_exited_ = true;
};
