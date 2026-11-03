#pragma once

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <functional>
#include <unordered_map>
#include <vector>

class CoroutinePool {
public:
    using OnCoroutineCreated = std::function<void(lua_State* co)>;

    CoroutinePool(lua_State* main_L, OnCoroutineCreated on_created);

    lua_State* Acquire();
    void Release(lua_State* co);

    // Release EVERY checked-out coroutine back to the pool. Only valid on the
    // executor thread at an instant where no pool coroutine is running or
    // mid-acquire — i.e. LuaRuntime::Interrupt()'s completion pass, where
    // every checked-out coroutine is abandoned by definition (parked on an
    // async op, hook-yielded out of a loop, or parked with no completion
    // handler at all). Without this they stay checked out forever: the
    // registry ref (and with it the coroutine's whole stack) is held until
    // Shutdown(), leaking on every interrupt of a reused runtime.
    void ReleaseAll();

    // True while co is checked out from this pool (between Acquire and
    // Release). Script-created coroutines are never pool-owned.
    [[nodiscard]] bool Owns(lua_State* co) const {
        return active_co_refs_.find(co) != active_co_refs_.end();
    }

    void Shutdown(lua_State* main_L);

private:
    std::unordered_map<lua_State*, int> active_co_refs_;
    std::vector<std::pair<lua_State*, int>> co_pool_;
    lua_State* main_L_;
    OnCoroutineCreated on_created_;
};
