#pragma once

#include <memory>

#include "behavior_tree_engine.h"
#include "lua_library.h"
#include "lua_runtime.h"

class Blackboard;
class ResourceProvider;

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

    void SetMainLibsPath(std::string path) { main_libs_path_ = std::move(path); }
    const std::string& main_libs_path() const { return main_libs_path_; }
    void SetProjectPath(std::string path) { project_path_ = std::move(path); }
    const std::string& project_path() const { return project_path_; }

    void SetResourceProvider(std::shared_ptr<ResourceProvider> provider) { resource_provider_ = std::move(provider); }
    std::shared_ptr<ResourceProvider> resource_provider() const { return resource_provider_; }

private:
    BehaviorTreeEngine::Ptr engine_;
    std::string project_path_;
    std::string main_libs_path_;
    std::shared_ptr<ResourceProvider> resource_provider_;
};
