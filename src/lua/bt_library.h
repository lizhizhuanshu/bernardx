#pragma once

#include <atomic>
#include <memory>

#include "behavior_tree_engine.h"
#include "lua_library.h"
#include "lua_runtime.h"

class Blackboard;

class BehaviorTreeLibrary : public LuaLibrary {
public:
    explicit BehaviorTreeLibrary(std::shared_ptr<Blackboard> bb);
    ~BehaviorTreeLibrary() override;

    BehaviorTreeLibrary(const BehaviorTreeLibrary&) = delete;
    BehaviorTreeLibrary& operator=(const BehaviorTreeLibrary&) = delete;
    BehaviorTreeLibrary(BehaviorTreeLibrary&&) = delete;
    BehaviorTreeLibrary& operator=(BehaviorTreeLibrary&&) = delete;

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

    AsyncHandle pending_run_handle() const { return pending_run_handle_; }
    void set_pending_run_handle(AsyncHandle h) { pending_run_handle_ = h; }
    LuaRuntime::Ptr pending_run_ctx() const { return pending_run_ctx_; }
    void set_pending_run_ctx(LuaRuntime::Ptr ctx) { pending_run_ctx_ = std::move(ctx); }
    bool run_completed() const { return run_completed_.load(); }
    void set_run_completed(bool v) { run_completed_.store(v); }
    void clear_pending_run() { pending_run_ctx_.reset(); pending_run_handle_ = 0; }

private:
    AsyncHandle pending_run_handle_ = 0;
    LuaRuntime::Ptr pending_run_ctx_;
    std::atomic<bool> run_completed_{false};
    BehaviorTreeEngine::Ptr engine_;

    int64_t tick_interval_ms_{100};
    std::string project_path_;
    std::string main_libs_path_;
};
