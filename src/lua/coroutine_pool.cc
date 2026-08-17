#include "coroutine_pool.h"

CoroutinePool::CoroutinePool(lua_State* main_L, OnCoroutineCreated on_created)
    : main_L_(main_L), on_created_(std::move(on_created)) {}

lua_State* CoroutinePool::Acquire() {
    if (!co_pool_.empty()) {
        auto [co, ref] = co_pool_.back();
        co_pool_.pop_back();
        active_co_refs_[co] = ref;
        return co;
    }

    lua_State* co = lua_newthread(main_L_);
    int ref = luaL_ref(main_L_, LUA_REGISTRYINDEX);
    if (on_created_) on_created_(co);
    active_co_refs_[co] = ref;
    return co;
}

void CoroutinePool::Release(lua_State* co) {
    // Reset a finished (or yielded-then-cancelled) coroutine back to a reusable
    // state. lua_settop only clears the stack; a coroutine that has returned
    // (status LUA_OK but not at base level) is "dead" and lua_resume on it
    // fails. lua_closethread unwinds the CallInfo and resets status to LUA_OK.
    lua_closethread(co, nullptr);
    auto it = active_co_refs_.find(co);
    if (it != active_co_refs_.end()) {
        co_pool_.push_back({co, it->second});
        active_co_refs_.erase(it);
    }
}

void CoroutinePool::Shutdown(lua_State* main_L) {
    for (auto& [co, ref] : active_co_refs_) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
    }
    active_co_refs_.clear();
    for (auto& [co, ref] : co_pool_) {
        luaL_unref(main_L, LUA_REGISTRYINDEX, ref);
    }
    co_pool_.clear();
}
