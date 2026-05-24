#pragma once

#include <cstddef>
#include <memory>
#include <vector>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"
#include "node.h"

class Composite : public Node {
public:
    void AddChild(std::unique_ptr<Node> child);

    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

    bool has_started() const { return current_child_index_ > 0; }
    size_t current_child_index() const { return current_child_index_; }

    void Reset() override;
    void OnAborted() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx,
                                         const std::string& base_path) override;

protected:
    Composite(uint32_t id, std::string type, std::string name);

    std::vector<std::unique_ptr<Node>> children_;
    size_t current_child_index_ = 0;
};
