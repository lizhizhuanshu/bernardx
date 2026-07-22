#pragma once

#include <memory>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

// Stores a per-library `shared_ptr<T>` on a lua_State via the registry, keyed
// by this instance's address, with a __gc'd userdata so the shared_ptr is
// released when the Lua state closes.
//
// Consolidates the registry-key + metatable + __gc + Set/Get boilerplate that
// was previously hand-written per library (e.g. http_library, async_io_library).
//
// The instance must outlive any lua_State that uses it — keep it at file scope
// or as a long-lived member. Each lua_State gets its own registry entry under
// the same key (the registry is per-state), matching the original semantics.
template <typename T>
class LibraryState {
public:
    explicit LibraryState(const char* metatable_name)
        : metatable_name_(metatable_name) {}

    LibraryState(const LibraryState&) = delete;
    LibraryState& operator=(const LibraryState&) = delete;

    void Set(lua_State* L, std::shared_ptr<T> state) const {
        EnsureMetatable(L);
        auto* slot = static_cast<std::shared_ptr<T>*>(
            lua_newuserdatauv(L, sizeof(std::shared_ptr<T>), 0));
        new (slot) std::shared_ptr<T>(std::move(state));
        luaL_setmetatable(L, metatable_name_);
        lua_rawsetp(L, LUA_REGISTRYINDEX, this);
    }

    std::shared_ptr<T> Get(lua_State* L) const {
        lua_rawgetp(L, LUA_REGISTRYINDEX, this);
        auto* slot = static_cast<std::shared_ptr<T>*>(lua_touserdata(L, -1));
        auto state = slot ? *slot : nullptr;
        lua_pop(L, 1);
        return state;
    }

private:
    void EnsureMetatable(lua_State* L) const {
        if (luaL_newmetatable(L, metatable_name_)) {
            lua_pushcfunction(L, &LibraryState::Gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_pop(L, 1);
    }

    static int Gc(lua_State* L) {
        auto* slot = static_cast<std::shared_ptr<T>*>(lua_touserdata(L, 1));
        if (slot) {
            slot->~shared_ptr<T>();
        }
        return 0;
    }

    const char* metatable_name_;
};
