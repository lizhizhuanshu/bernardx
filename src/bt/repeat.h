#pragma once

#include <memory>
#include <string>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"
#include "node.h"

class Repeat : public Node {
public:
    static constexpr int kInfinite = -1;

    Repeat(uint32_t id, std::string name, int count,
           std::unique_ptr<Node> child);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx,
                                         const std::string& base_path) override;
    void ReleaseRefs() override;

private:
    std::unique_ptr<Node> child_;
    int max_count_;
    int current_count_ = 0;
};
