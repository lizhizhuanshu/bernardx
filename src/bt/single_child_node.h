#pragma once

#include <memory>
#include <string>

extern "C" {
#include "lua.h"
}

#include <async_simple/coro/Lazy.h>

#include "lua_runtime.h"
#include "node.h"

class SingleChildNode : public Node {
public:
    void Reset() override;
    void OnAborted() override;
    async_simple::coro::Lazy<bool> Init(lua_State* L, LuaRuntime* ctx,
                                         const std::string& base_path) override;

    Node* child() const { return child_.get(); }

protected:
    SingleChildNode(uint32_t id, std::string type, std::string name,
                    std::unique_ptr<Node> child);

    std::unique_ptr<Node> child_;
};
