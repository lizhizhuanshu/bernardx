#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "behavior_tree_engine.h"
#include "lua_context.h"
#include "lua_library.h"

namespace sol {
class state;
}

class BehaviorTreeLibrary : public LuaLibrary {
public:
    BehaviorTreeLibrary();
    ~BehaviorTreeLibrary() override;

    std::string name() const override { return "bt"; }
    void Open(lua_State* L) override;
    void Close(lua_State* L) override;

    BehaviorTreeEngine::Ptr engine() const { return engine_; }

    void SetTickIntervalMs(int64_t ms) { tick_interval_ms_ = ms; }
    int64_t tick_interval_ms() const { return tick_interval_ms_; }

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
    LuaContext::Ptr pending_run_ctx_;
    std::atomic<bool> run_completed_{false};

private:
    void BtEventLoop();

    BehaviorTreeEngine::Ptr engine_;

    std::unique_ptr<sol::state> bt_lua_;
    LuaContext::Ptr bt_context_;

    std::thread bt_thread_;
    std::atomic<bool> bt_running_{false};
    int64_t tick_interval_ms_ = 100;

    std::string project_path_;
    std::string main_libs_path_;

    // on_complete_ is set on LuaRuntime thread before BT thread starts,
    // read only on BT thread. Thread creation provides the happens-before.
    CompletionCallback on_complete_;
};
