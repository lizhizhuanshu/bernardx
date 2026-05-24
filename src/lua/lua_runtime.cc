#include "lua_runtime.h"

#include <chrono>
#include <cstring>

#include <async_simple/executors/SimpleExecutor.h>

#include <spdlog/spdlog.h>

#include "lua_value_utils.h"
#include "time_utils.h"

namespace {
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

async_simple::coro::Lazy<void> ResumeModuleLoad(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    std::string module_name) {
    auto source = co_await rt->code_provider()->LoadModule(module_name);
    if (source.has_value()) {
        rt->PushRequireRun(handle, std::move(*source), std::move(module_name));
    } else {
        rt->PushResume(handle, {LuaValue{nullptr}});
    }
}

async_simple::coro::Lazy<void> ResumeFileLoad(
    std::shared_ptr<LuaRuntime> rt,
    AsyncHandle handle,
    std::string file_path) {
    auto source = co_await rt->code_provider()->LoadFile(file_path);
    if (source.has_value()) {
        rt->PushLoadFileRun(handle, std::move(*source), std::move(file_path));
    } else {
        rt->PushResume(handle, {LuaValue{nullptr}});
    }
}

// Cache module result: non-nil values cached as-is, nil replaced with true (matches native Lua)
void CacheModuleResult(lua_State* L, const char* name, int result_idx) {
    if (result_idx < 0) result_idx = lua_absindex(L, result_idx);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (lua_isnil(L, result_idx)) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushvalue(L, result_idx);
    }
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void CacheModuleValues(lua_State* L, const char* name, std::vector<LuaValue>& values) {
    if (values.empty()) values.push_back(nullptr);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (std::holds_alternative<std::nullptr_t>(values[0])) {
        lua_pushboolean(L, 1);
    } else {
        LuaRuntime::PushValues(L, values);
    }
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

// --- custom_require continuation ---

int require_continuation(lua_State* L, int status, lua_KContext ctx) {
    const char* name = lua_tostring(L, 1);
    if (lua_isnil(L, 2)) {
        if (lua_isstring(L, 3)) {
            return luaL_error(L, "error loading module '%s':\n\t%s",
                              name, lua_tostring(L, 3));
        }
        return luaL_error(L, "module '%s' not found", name);
    }
    lua_remove(L, 1);
    return 1;
}

// --- custom_loadfile continuation ---

int loadfile_continuation(lua_State* L, int status, lua_KContext ctx) {
    if (lua_isnil(L, 2)) {
        const char* filename = lua_tostring(L, 1);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushfstring(L, "cannot find file '%s' via CodeProvider", filename);
        return 2;
    }
    lua_remove(L, 1);
    return 1;
}

// --- Custom require ---

int custom_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (lua_toboolean(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 2);

    auto rt = LuaRuntime::FromLuaState(L);

    auto openf = rt->find_c_module(name);
    if (openf.has_value()) {
        lua_pushcfunction(L, *openf);
        lua_pushstring(L, name);
        int call_status = lua_pcall(L, 1, 1, 0);
        if (call_status == LUA_YIELD) {
            return luaL_error(L, "C module '%s' attempted to yield during loading", name);
        }
        if (call_status != LUA_OK) {
            return lua_error(L);
        }
        CacheModuleResult(L, name, -1);
        return 1;
    }

    auto lib = rt->find_library(name);
    if (lib) {
        int top = lua_gettop(L);
        lib->Open(L);
        if (lua_gettop(L) != top + 1) {
            return luaL_error(L, "library '%s' Open() must push exactly 1 value", name);
        }
        CacheModuleResult(L, name, -1);
        return 1;
    }

    if (rt->code_provider()) {
        if (!rt->executor()) {
            return luaL_error(L, "module '%s': executor required for CodeProvider", name);
        }

        auto handle = rt->PreYield(L);
        auto* exec = rt->executor();
        ResumeModuleLoad(rt, handle, std::string(name)).via(exec).detach();

        return lua_yieldk(L, 0, 0, require_continuation);
    }

    return luaL_error(L, "module '%s' not found", name);
}

// --- Custom loadfile ---

int custom_loadfile(lua_State* L) {
    const char* filename = luaL_checkstring(L, 1);
    if (filename[0] == '\0') {
        lua_pushnil(L);
        lua_pushliteral(L, "empty filename");
        return 2;
    }

    if (filename[0] == '/') {
        int status = luaL_loadfile(L, filename);
        if (status != LUA_OK) {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        return 1;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    if (!rt || !rt->code_provider()) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load relative file '%s': no CodeProvider", filename);
        return 2;
    }
    if (!rt->executor()) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load relative file '%s': no executor", filename);
        return 2;
    }

    auto handle = rt->PreYield(L);
    auto* exec = rt->executor();
    ResumeFileLoad(rt, handle, std::string(filename)).via(exec).detach();

    return lua_yieldk(L, 0, 0, loadfile_continuation);
}

// --- Built-in functions ---

int builtin_now(lua_State* L) {
    lua_pushinteger(L, NowMs());
    return 1;
}

int builtin_sleep(lua_State* L) {
    int ms = static_cast<int>(luaL_checkinteger(L, 1));
    auto rt = LuaRuntime::FromLuaState(L);
    auto handle = rt->PreYield(L);
    rt->AddSleepTimer(NowMs() + ms, handle);
    return LuaRuntime::Yield(L);
}

int builtin_set_timeout(lua_State* L) {
    int ms = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto rt = LuaRuntime::FromLuaState(L);

    lua_State* main_L = rt->main_state();
    lua_pushvalue(L, 2);
    lua_xmove(L, main_L, 1);
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto handle = rt->AddTimeoutTimer(NowMs() + ms, fn_ref);
    lua_pushinteger(L, static_cast<lua_Integer>(handle));
    return 1;
}

int builtin_clear_timeout(lua_State* L) {
    auto rt = LuaRuntime::FromLuaState(L);
    auto handle = static_cast<AsyncHandle>(luaL_checkinteger(L, 1));
    rt->CancelTimer(handle);
    return 0;
}

// --- await(resolve, reject) ---

enum { UV_CTX_PTR = 1, UV_HANDLE = 2, UV_DONE_TABLE = 3 };

const char* kAwaitCtxMetatable = "await__ctx";

int await_ctx_gc(lua_State* L) {
    auto* ptr = static_cast<std::shared_ptr<LuaRuntime>*>(lua_touserdata(L, 1));
    ptr->~shared_ptr();
    return 0;
}

// Push a shared_ptr<LuaRuntime> as a GC-managed userdata onto the stack.
void push_runtime_ptr(lua_State* L, const LuaRuntime::Ptr& rt) {
    auto* slot = static_cast<std::shared_ptr<LuaRuntime>*>(lua_newuserdatauv(L, sizeof(std::shared_ptr<LuaRuntime>), 0));
    new (slot) std::shared_ptr<LuaRuntime>(rt);
    luaL_setmetatable(L, kAwaitCtxMetatable);
}

std::shared_ptr<LuaRuntime> get_runtime_ptr(lua_State* L, int upvalue_idx) {
    auto* slot = static_cast<std::shared_ptr<LuaRuntime>*>(lua_touserdata(L, lua_upvalueindex(upvalue_idx)));
    return *slot;
}

int builtin_await_resolve(lua_State* L) {
    lua_rawgeti(L, lua_upvalueindex(UV_DONE_TABLE), 1);
    bool done = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (done) return 0;

    lua_pushboolean(L, 1);
    lua_rawseti(L, lua_upvalueindex(UV_DONE_TABLE), 1);

    auto rt = get_runtime_ptr(L, UV_CTX_PTR);
    auto handle = static_cast<AsyncHandle>(lua_tointeger(L, lua_upvalueindex(UV_HANDLE)));

    int nargs = lua_gettop(L);
    auto values = LuaRuntime::PeekValues(L, nargs);
    rt->PushResume(handle, std::move(values));
    return 0;
}

int builtin_await_reject(lua_State* L) {
    lua_rawgeti(L, lua_upvalueindex(UV_DONE_TABLE), 1);
    bool done = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (done) return 0;

    lua_pushboolean(L, 1);
    lua_rawseti(L, lua_upvalueindex(UV_DONE_TABLE), 1);

    auto rt = get_runtime_ptr(L, UV_CTX_PTR);
    auto handle = static_cast<AsyncHandle>(lua_tointeger(L, lua_upvalueindex(UV_HANDLE)));

    std::vector<LuaValue> args;
    args.push_back(nullptr);
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        size_t len;
        const char* s = lua_tolstring(L, 1, &len);
        args.push_back(std::string(s, len));
    } else {
        args.push_back(std::string("rejected"));
    }
    rt->PushResume(handle, std::move(args));
    return 0;
}

int builtin_await(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    auto rt = LuaRuntime::FromLuaState(L);

    auto handle = rt->PreYield(L);

    lua_newtable(L);

    push_runtime_ptr(L, rt);
    lua_pushinteger(L, handle);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, builtin_await_resolve, 3);

    push_runtime_ptr(L, rt);
    lua_pushinteger(L, handle);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, builtin_await_reject, 3);

    lua_pushvalue(L, 1);
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushvalue(L, 3);
    int resolve_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushvalue(L, 4);
    int reject_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    async_simple::Promise<ScriptResult> promise;
    rt->PushTask({CallRef{fn_ref,
        {rt->CreateRef(resolve_ref, LUA_TFUNCTION), rt->CreateRef(reject_ref, LUA_TFUNCTION)},
        true}, std::move(promise)});

    return LuaRuntime::Yield(L);
}

}  // namespace

// --- LuaRuntime implementation ---

LuaRuntime::LuaRuntime() : lua_(std::make_unique<sol::state>()), main_L_(lua_->lua_state()) {
    SetExtraspace(main_L_, this);
    lua_->open_libraries();
}

LuaRuntime::~LuaRuntime() {
    // Shutdown first (calls extensions/libraries OnShutdown/Close while Lua state alive).
    Shutdown();
    // Then destroy executor (joins its thread) — no more tasks can reference Lua state.
    owned_executor_.reset();
}

// --- LuaRef ---

struct LuaRefImpl : public LuaRefBase {
    ~LuaRefImpl() override {
        auto rt = rt_.lock();
        if (rt && ref != LUA_NOREF) {
            rt->PushRelease({ref});
        }
    }
private:
    std::weak_ptr<LuaRuntime> rt_;
    LuaRefImpl(int r, int t, std::weak_ptr<LuaRuntime> rt)
        : LuaRefBase(r, t), rt_(std::move(rt)) {}
    friend class LuaRuntime;
};

LuaRef LuaRuntime::CreateRef(int ref, int type) {
    return LuaRef(new LuaRefImpl(ref, type, shared_from_this()));
}

// --- Extraspace ---

void LuaRuntime::SetExtraspace(lua_State* L, LuaRuntime* rt) {
    std::memcpy(lua_getextraspace(L), &rt, sizeof(LuaRuntime*));
}

LuaRuntime::Ptr LuaRuntime::FromLuaState(lua_State* L) {
    auto** rt_ptr = reinterpret_cast<LuaRuntime**>(lua_getextraspace(L));
    return rt_ptr && *rt_ptr ? (*rt_ptr)->shared_from_this() : nullptr;
}

// --- Configuration accessors ---

std::optional<lua_CFunction> LuaRuntime::find_c_module(const std::string& name) const {
    auto it = c_modules_.find(name);
    return it != c_modules_.end() ? std::optional(it->second) : std::nullopt;
}

std::shared_ptr<LuaLibrary> LuaRuntime::find_library(const std::string& name) const {
    auto it = libraries_.find(name);
    return it != libraries_.end() ? it->second : nullptr;
}

// --- Setup ---

void LuaRuntime::Setup(lua_State* main_L) {
    SetupBuiltins(main_L);

    for (size_t i = 0; i < extensions_.size(); ++i) {
        try {
            extensions_[i]->OnInit(main_L);
        } catch (...) {
            for (size_t j = 0; j < i; ++j) {
                try {
                    extensions_[j]->OnShutdown(main_L);
                } catch (...) {
                }
            }
            throw;
        }
    }

    SetupCustomRequire(main_L);
}

void LuaRuntime::SetupBuiltins(lua_State* main_L) {
    // Register metatable for await context userdata (shared_ptr<LuaRuntime> with __gc)
    luaL_newmetatable(main_L, kAwaitCtxMetatable);
    lua_pushcfunction(main_L, await_ctx_gc);
    lua_setfield(main_L, -2, "__gc");
    lua_pop(main_L, 1);

    lua_pushcfunction(main_L, builtin_now);
    lua_setglobal(main_L, "now");

    lua_pushcfunction(main_L, builtin_sleep);
    lua_setglobal(main_L, "sleep");

    lua_pushcfunction(main_L, builtin_set_timeout);
    lua_setglobal(main_L, "setTimeout");

    lua_pushcfunction(main_L, builtin_clear_timeout);
    lua_setglobal(main_L, "clearTimeout");

    lua_pushcfunction(main_L, builtin_await);
    lua_setglobal(main_L, "await");
}

void LuaRuntime::SetupCustomRequire(lua_State* main_L) {
    if (!code_provider_ && c_modules_.empty() && libraries_.empty()) return;

    lua_getglobal(main_L, "package");
    lua_newtable(main_L);
    lua_setfield(main_L, -2, "searchers");
    lua_pop(main_L, 1);

    lua_pushcfunction(main_L, custom_require);
    lua_setglobal(main_L, "require");

    if (code_provider_) {
        lua_pushcfunction(main_L, custom_loadfile);
        lua_setglobal(main_L, "loadfile");
    }
}

// --- Thread-safe submission via executor ---

void LuaRuntime::PushTask(TaskRequest task) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        task.promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
        return;
    }
    auto* self = this;
    executor_->schedule([self, task = std::move(task)]() mutable {
        if (self->shutting_down_.load(std::memory_order_acquire)) {
            task.promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
            return;
        }
        self->ProcessTask(std::move(task));
    });
}

void LuaRuntime::PushResume(AsyncHandle handle, std::vector<LuaValue> args) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    auto* self = this;
    executor_->schedule([self, handle, args = std::move(args)]() mutable {
        if (self->shutting_down_.load(std::memory_order_acquire)) return;
        self->DoResume(handle, std::move(args));
    });
}

void LuaRuntime::PushRelease(std::vector<int> refs) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    auto* self = this;
    executor_->schedule([self, refs = std::move(refs)]() {
        for (int ref : refs) {
            luaL_unref(self->main_L_, LUA_REGISTRYINDEX, ref);
        }
    });
}

void LuaRuntime::PushRequireRun(AsyncHandle handle, std::string source, std::string module_name) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    auto* self = this;
    executor_->schedule([self, handle, source = std::move(source), module_name = std::move(module_name)]() mutable {
        if (self->shutting_down_.load(std::memory_order_acquire)) return;
        self->ProcessRequireRun(std::move(source), std::move(module_name), handle);
    });
}

void LuaRuntime::PushLoadFileRun(AsyncHandle handle, std::string source, std::string filename) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    auto* self = this;
    executor_->schedule([self, handle, source = std::move(source), filename = std::move(filename)]() mutable {
        if (self->shutting_down_.load(std::memory_order_acquire)) return;
        self->ProcessLoadFileRun(std::move(source), std::move(filename), handle);
    });
}

void LuaRuntime::CallLuaFunction(int fn_ref, std::vector<LuaValue> args) {
    PushTask({CallRef{fn_ref, std::move(args), false}, {}});
}

// --- Task processing (executor thread only) ---

void LuaRuntime::ProcessTask(TaskRequest task) {
    lua_State* co = AcquireCo();
    int nargs = 0;

    std::visit(overloaded{
                   [&](const LoadScript& s) {
                       int load_result;
                       if (s.chunk.empty()) {
                           load_result = luaL_loadfile(co, s.name.c_str());
                       } else {
                           load_result = luaL_loadbuffer(co, s.chunk.c_str(), s.chunk.size(), s.name.c_str());
                       }
                       if (load_result != LUA_OK) {
                           const char* err = lua_tostring(co, -1);
                           spdlog::error("LuaRuntime: {}", err ? err : "unknown error");
                           lua_pop(co, 1);
                           ReleaseCo(co);
                           ScriptResult result;
                           result.status = load_result;
                           result.error = err ? err : "unknown error";
                           task.promise.setValue(std::move(result));
                           co = nullptr;
                       }
                   },
                   [&](const CallRef& c) {
                       lua_rawgeti(co, LUA_REGISTRYINDEX, c.fn_ref);
                       if (c.auto_unref) {
                           luaL_unref(main_L_, LUA_REGISTRYINDEX, c.fn_ref);
                       }
                       PushValues(co, c.args);
                       nargs = static_cast<int>(c.args.size());
                   }},
               task.kind);

    if (!co) return;

    int nresults = 0;
    int status = lua_resume(co, main_L_, nargs, &nresults);

    script_promises_[co] = std::move(task.promise);

    if (status != LUA_YIELD) {
        MaybeRecycleCo(co, status, nresults);
    }
}

void LuaRuntime::ProcessRequireRun(std::string source, std::string module_name, AsyncHandle caller_handle) {
    lua_State* co = AcquireCo();

    std::string chunkname = "@" + module_name;
    int load_status = luaL_loadbuffer(co, source.c_str(), source.size(), chunkname.c_str());
    if (load_status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        std::string errmsg = err ? err : "unknown error";
        spdlog::error("RequireRun: {}", errmsg);
        lua_pop(co, 1);
        ReleaseCo(co);
        PushResume(caller_handle, {LuaValue{nullptr}, LuaValue{std::move(errmsg)}});
        return;
    }

    lua_pushstring(co, module_name.c_str());
    int nresults = 0;
    int status = lua_resume(co, main_L_, 1, &nresults);

    if (status == LUA_OK) {
        auto values = PeekValues(co, nresults);
        CacheModuleValues(main_L_, module_name.c_str(), values);
        PushResume(caller_handle, std::move(values));
        ReleaseCo(co);
    } else if (status == LUA_YIELD) {
        auto handle = caller_handle;
        auto mod_name = module_name;
        auto self = shared_from_this();
        SetCoCompleteCallback(co,
            [self, handle, mod_name](ScriptResult result) {
                if (result.status == LUA_OK) {
                    auto& vals = result.values;
                    CacheModuleValues(self->main_state(), mod_name.c_str(), vals);
                    self->PushResume(handle, std::move(vals));
                } else {
                    auto err = result.error.empty()
                        ? "unknown error" : std::move(result.error);
                    self->PushResume(handle,
                        {LuaValue{nullptr}, LuaValue{std::move(err)}});
                }
            });
    } else {
        const char* err = lua_tostring(co, -1);
        std::string errmsg = err ? err : "unknown error";
        spdlog::error("RequireRun: {}", errmsg);
        lua_pop(co, 1);
        PushResume(caller_handle,
            {LuaValue{nullptr}, LuaValue{std::move(errmsg)}});
        ReleaseCo(co);
    }
}

void LuaRuntime::ProcessLoadFileRun(std::string source, std::string filename, AsyncHandle caller_handle) {
    lua_State* co = AcquireCo();

    std::string chunkname = "@" + filename;
    int load_status = luaL_loadbuffer(co, source.c_str(), source.size(), chunkname.c_str());
    if (load_status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        spdlog::error("LoadFileRun: {}", err ? err : "unknown error");
        lua_pop(co, 1);
        ReleaseCo(co);
        PushResume(caller_handle, {LuaValue{nullptr}});
        return;
    }

    lua_pushvalue(co, -1);
    int ref = luaL_ref(co, LUA_REGISTRYINDEX);
    int type = lua_type(co, -1);
    PushResume(caller_handle, {CreateRef(ref, type)});
    ReleaseCo(co);
}

// --- Yield support ---

AsyncHandle LuaRuntime::PreYield(lua_State* co) {
    auto handle = timer_mgr_->NextHandle();
    pending_[handle] = {co};
    return handle;
}

int LuaRuntime::Yield(lua_State* L) {
    return lua_yield(L, 0);
}

// --- Value marshalling ---

std::vector<LuaValue> LuaRuntime::PeekValues(lua_State* L, int nresults) {
    std::vector<LuaValue> result;
    result.reserve(nresults);
    for (int i = -nresults; i < 0; ++i) {
        result.push_back(LuaValueFromStack(L, i));
    }
    return result;
}

void LuaRuntime::PushValues(lua_State* L, const std::vector<LuaValue>& values) {
    for (const auto& v : values) {
        std::visit(
            [L](const auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) {
                    lua_pushnil(L);
                } else if constexpr (std::is_same_v<T, bool>) {
                    lua_pushboolean(L, val ? 1 : 0);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    lua_pushinteger(L, static_cast<lua_Integer>(val));
                } else if constexpr (std::is_same_v<T, double>) {
                    lua_pushnumber(L, static_cast<lua_Number>(val));
                } else if constexpr (std::is_same_v<T, std::string>) {
                    lua_pushlstring(L, val.c_str(), val.size());
                } else if constexpr (std::is_same_v<T, LuaRef>) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, val->ref);
                }
            },
            v);
    }
}

// --- Shutdown ---

void LuaRuntime::Shutdown() {
    if (shutting_down_.exchange(true, std::memory_order_acq_rel)) return;

    decltype(script_promises_) pending_promises;

    std::swap(script_promises_, pending_promises);
    pending_.clear();
    co_complete_callbacks_.clear();

    for (auto& ext : extensions_) {
        ext->OnShutdown(main_L_);
    }
    for (auto& [name, lib] : libraries_) {
        lib->Close(main_L_);
    }

    for (auto& [co, promise] : pending_promises) {
        promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
    }

    if (timer_mgr_) {
        timer_mgr_->CancelAll(main_L_);
    }
    if (co_pool_) {
        co_pool_->Shutdown(main_L_);
    }
    SetExtraspace(main_L_, nullptr);
}

// --- Coroutine completion callback ---

void LuaRuntime::SetCoCompleteCallback(lua_State* co, CoCompleteCallback cb) {
    co_complete_callbacks_[co] = std::move(cb);
}

void LuaRuntime::RemoveCoCompleteCallback(lua_State* co) {
    co_complete_callbacks_.erase(co);
}

void LuaRuntime::MaybeRecycleCo(lua_State* co, int status, int nresults) {
    std::string error_msg;
    if (status != LUA_OK && status != LUA_YIELD) {
        const char* err = lua_tostring(co, -1);
        error_msg = err ? err : "unknown error";
        spdlog::error("LuaRuntime: {}", error_msg);
        lua_pop(co, 1);
    }
    if (status != LUA_YIELD) {
        // Build the result once
        ScriptResult result;
        result.status = status;
        result.error = std::move(error_msg);
        if (status == LUA_OK) {
            result.values = PeekValues(co, nresults);
        }

        // Save and erase the promise BEFORE any callback to avoid data race.
        std::optional<async_simple::Promise<ScriptResult>> saved_promise;
        auto it = script_promises_.find(co);
        if (it != script_promises_.end()) {
            saved_promise = std::move(it->second);
            script_promises_.erase(it);
        }

        // Invoke completion callback with a copy (callback may also need values)
        auto cb_it = co_complete_callbacks_.find(co);
        if (cb_it != co_complete_callbacks_.end()) {
            cb_it->second(ScriptResult{result});  // copy
            co_complete_callbacks_.erase(cb_it);
        }

        ReleaseCo(co);

        // LAST: resolve the promise — this wakes the main thread
        if (saved_promise) {
            saved_promise->setValue(std::move(result));
        }
    }
}

LuaRuntime::ResumeResult LuaRuntime::DoResume(AsyncHandle handle, std::vector<LuaValue> args) {
    lua_State* co = nullptr;
    auto it = pending_.find(handle);
    if (it == pending_.end()) {
        spdlog::error("LuaRuntime::DoResume: invalid handle {}", handle);
        return {nullptr, LUA_ERRRUN};
    }
    co = it->second.co;
    pending_.erase(it);

    PushValues(co, args);
    int nresults = 0;
    int status = lua_resume(co, main_L_, static_cast<int>(args.size()), &nresults);
    spdlog::debug("DoResume handle={}: lua_resume status={}", handle, status);
    MaybeRecycleCo(co, status, nresults);
    return {co, status};
}

// --- Public script API ---

async_simple::coro::Lazy<ScriptResult> LuaRuntime::RunScript(const std::string& script) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    PushTask({LoadScript{script, "=script"}, std::move(promise)});
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::RunFile(const std::string& filename) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    PushTask({LoadScript{"", filename}, std::move(promise)});
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::CallFunction(int fn_ref, std::vector<LuaValue> args) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    PushTask({CallRef{fn_ref, std::move(args), false}, std::move(promise)});
    co_return co_await std::move(future);
}

// --- Async coroutine API (executor thread only) ---

async_simple::coro::Lazy<ScriptResult> LuaRuntime::AwaitCoroutine(lua_State* co, int status, int nresults) {
    if (status == LUA_OK) {
        auto values = PeekValues(co, nresults);
        ReleaseCo(co);
        co_return ScriptResult{LUA_OK, std::move(values)};
    }

    if (status == LUA_YIELD) {
        async_simple::Promise<ScriptResult> promise;
        auto future = promise.getFuture();

        SetCoCompleteCallback(co, [p = std::move(promise)](ScriptResult result) mutable {
            p.setValue(std::move(result));
        });

        co_return co_await std::move(future);
    }

    // Error
    const char* err = lua_tostring(co, -1);
    std::string errmsg = err ? err : "unknown error";
    spdlog::error("LuaRuntime: {}", errmsg);
    lua_pop(co, 1);
    ReleaseCo(co);
    co_return ScriptResult{status, {}, std::move(errmsg)};
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::CallAsync(int fn_ref, std::vector<LuaValue> args) {
    lua_State* co = AcquireCo();

    lua_rawgeti(co, LUA_REGISTRYINDEX, fn_ref);
    PushValues(co, args);
    int nargs = static_cast<int>(args.size());

    int nresults = 0;
    int status = lua_resume(co, main_L_, nargs, &nresults);

    co_return co_await AwaitCoroutine(co, status, nresults);
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::DoFileAsync(const std::string& path) {
    lua_State* co = AcquireCo();

    int load_status = luaL_loadfile(co, path.c_str());
    if (load_status != LUA_OK) {
        const char* err = lua_tostring(co, -1);
        std::string errmsg = err ? err : "unknown error";
        spdlog::error("LuaRuntime::DoFileAsync: {}", errmsg);
        lua_pop(co, 1);
        ReleaseCo(co);
        co_return ScriptResult{load_status, {}, std::move(errmsg)};
    }

    int nresults = 0;
    int status = lua_resume(co, main_L_, 0, &nresults);

    co_return co_await AwaitCoroutine(co, status, nresults);
}

// --- LuaRuntime::Builder ---

LuaRuntime::Builder& LuaRuntime::Builder::WithCodeProvider(std::shared_ptr<CodeProvider> provider) {
    code_provider_ = std::move(provider);
    return *this;
}

LuaRuntime::Builder& LuaRuntime::Builder::WithExecutor(async_simple::Executor& executor) {
    executor_ = &executor;
    return *this;
}

LuaRuntime::Builder& LuaRuntime::Builder::Register(const std::string& name, lua_CFunction openf) {
    c_modules_[name] = openf;
    return *this;
}

LuaRuntime::Builder& LuaRuntime::Builder::RegisterExtension(std::shared_ptr<LuaExtension> extension) {
    extensions_.push_back(std::move(extension));
    return *this;
}

LuaRuntime::Builder& LuaRuntime::Builder::RegisterLibrary(std::shared_ptr<LuaLibrary> library) {
    libraries_[library->name()] = std::move(library);
    return *this;
}

LuaRuntime::Builder& LuaRuntime::Builder::InheritFrom(LuaRuntime* parent) {
    if (!parent) return *this;
    for (auto& [name, openf] : parent->c_modules_) {
        c_modules_[name] = openf;
    }
    for (auto& [name, lib] : parent->libraries_) {
        libraries_[name] = lib;  // shared_ptr copy — shared ownership
    }
    for (auto& ext : parent->extensions_) {
        extensions_.push_back(ext);  // shared ownership
    }
    return *this;
}

LuaRuntime::Ptr LuaRuntime::Builder::Create() {
    auto rt = std::shared_ptr<LuaRuntime>(new LuaRuntime());

    if (!executor_) {
        rt->owned_executor_ = std::make_unique<async_simple::executors::SimpleExecutor>(1);
        rt->executor_ = rt->owned_executor_.get();
    } else {
        rt->executor_ = executor_;
    }

    auto* self = rt.get();
    rt->co_pool_ = std::make_unique<CoroutinePool>(rt->main_L_,
        [self](lua_State* co) { LuaRuntime::SetExtraspace(co, self); });
    rt->timer_mgr_ = std::make_unique<TimerManager>(
        rt->executor_,
        [self](AsyncHandle handle) { self->DoResume(handle, {}); },
        [self](int fn_ref) {
            lua_State* co = self->AcquireCo();
            lua_rawgeti(co, LUA_REGISTRYINDEX, fn_ref);
            luaL_unref(self->main_L_, LUA_REGISTRYINDEX, fn_ref);
            int nresults = 0;
            int status = lua_resume(co, self->main_L_, 0, &nresults);
            if (status != LUA_YIELD) {
                self->MaybeRecycleCo(co, status, nresults);
            }
        },
        [self](int fn_ref) {
            luaL_unref(self->main_L_, LUA_REGISTRYINDEX, fn_ref);
        });

    rt->SetCodeProvider(code_provider_);
    rt->SetExecutor(rt->executor_);
    rt->SetCModules(c_modules_);
    rt->SetLibraries(libraries_);
    rt->SetExtensions(extensions_);

    rt->Setup(rt->main_L_);
    return rt;
}
