#include "lua_context.h"

#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

namespace {
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

async_simple::coro::Lazy<void> ResumeModuleLoad(
    std::shared_ptr<LuaContext> ctx,
    AsyncHandle handle,
    std::string module_name) {
    auto source = co_await ctx->code_provider()->LoadModule(module_name);
    if (source.has_value()) {
        // Push a RequireRun task: compile + run chunk in a fresh coroutine via lua_resume
        ctx->PushRequireRun(handle, std::move(*source), std::move(module_name));
    } else {
        // Module not found, resume caller with nil
        ctx->PushResume(handle, {LuaValue{nullptr}});
    }
}

async_simple::coro::Lazy<void> ResumeFileLoad(
    std::shared_ptr<LuaContext> ctx,
    AsyncHandle handle,
    std::string file_path) {
    auto source = co_await ctx->code_provider()->LoadFile(file_path);
    if (source.has_value()) {
        ctx->PushLoadFileRun(handle, std::move(*source), std::move(file_path));
    } else {
        ctx->PushResume(handle, {LuaValue{nullptr}});
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

// Cache a LuaValue vector into package.loaded[name] on the given state
void CacheModuleValues(lua_State* L, const char* name, std::vector<LuaValue>& values) {
    if (values.empty()) values.push_back(nullptr);
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (std::holds_alternative<std::nullptr_t>(values[0])) {
        lua_pushboolean(L, 1);
    } else {
        LuaContext::PushValues(L, values);
    }
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

// --- custom_require continuation (called after RequireRun completes) ---

// Stack when called: [name] [result_or_nil] [error_or_nil]
int require_continuation(lua_State* L, int status, lua_KContext ctx) {
    const char* name = lua_tostring(L, 1);

    if (lua_isnil(L, 2)) {
        if (lua_isstring(L, 3)) {
            return luaL_error(L, "error loading module '%s':\n\t%s",
                              name, lua_tostring(L, 3));
        }
        return luaL_error(L, "module '%s' not found", name);
    }

    lua_remove(L, 1);  // remove name, leaving [result]
    return 1;
}

// --- custom_loadfile continuation (called after LoadFileRun completes) ---

// Stack when called: [filename] [chunk_or_nil]
int loadfile_continuation(lua_State* L, int status, lua_KContext ctx) {
    if (lua_isnil(L, 2)) {
        const char* filename = lua_tostring(L, 1);
        lua_pop(L, 1);  // remove nil
        lua_pushnil(L);
        lua_pushfstring(L, "cannot find file '%s' via CodeProvider", filename);
        return 2;
    }
    lua_remove(L, 1);  // remove filename, leaving [chunk]
    return 1;
}

// --- Custom require ---

int custom_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    // 1. Check package.loaded cache
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, name);
    if (lua_toboolean(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 2);

    auto ctx = LuaContext::FromLuaState(L);

    // 2. Check C modules (synchronous)
    auto openf = ctx->find_c_module(name);
    if (openf.has_value()) {
        lua_pushcfunction(L, *openf);
        lua_pushstring(L, name);
        int call_status = lua_pcall(L, 1, 1, 0);
        if (call_status == LUA_YIELD) {
            return luaL_error(L, "C module '%s' attempted to yield during loading", name);
        }
        if (call_status != LUA_OK) {
            return lua_error(L);  // error message already on stack
        }
        CacheModuleResult(L, name, -1);
        return 1;
    }

    // 2.5. Check LuaLibrary (synchronous, Open pushes a table)
    auto lib = ctx->find_library(name);
    if (lib) {
        int top = lua_gettop(L);
        lib->Open(L);
        if (lua_gettop(L) != top + 1) {
            return luaL_error(L, "library '%s' Open() must push exactly 1 value", name);
        }
        CacheModuleResult(L, name, -1);
        return 1;
    }

    // 3. CodeProvider (async via async_simple + yieldk)
    if (ctx->code_provider()) {
        if (!ctx->executor()) {
            return luaL_error(L, "module '%s': executor required for CodeProvider", name);
        }

        auto handle = ctx->PreYield(L);
        auto* exec = ctx->executor();
        ResumeModuleLoad(ctx, handle, std::string(name)).via(exec).detach();

        return lua_yieldk(L, 0, 0, require_continuation);
    }

    // 4. Not found
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

    auto ctx = LuaContext::FromLuaState(L);
    if (!ctx || !ctx->code_provider()) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load relative file '%s': no CodeProvider", filename);
        return 2;
    }
    if (!ctx->executor()) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load relative file '%s': no executor", filename);
        return 2;
    }

    auto handle = ctx->PreYield(L);
    auto* exec = ctx->executor();
    ResumeFileLoad(ctx, handle, std::string(filename)).via(exec).detach();

    return lua_yieldk(L, 0, 0, loadfile_continuation);
}

// --- Built-in functions ---

int lua_now(lua_State* L) {
    lua_pushinteger(L, NowMs());
    return 1;
}

int lua_sleep(lua_State* L) {
    int ms = static_cast<int>(luaL_checkinteger(L, 1));
    auto ctx = LuaContext::FromLuaState(L);
    auto handle = ctx->PreYield(L);
    ctx->AddSleepTimer(NowMs() + ms, handle);
    return LuaContext::Yield(L);
}

int lua_set_timeout(lua_State* L) {
    int ms = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto ctx = LuaContext::FromLuaState(L);

    lua_State* main_L = ctx->main_state();
    lua_pushvalue(L, 2);
    lua_xmove(L, main_L, 1);
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto handle = ctx->AddTimeoutTimer(NowMs() + ms, fn_ref);
    lua_pushinteger(L, static_cast<lua_Integer>(handle));
    return 1;
}

int lua_clear_timeout(lua_State* L) {
    auto ctx = LuaContext::FromLuaState(L);
    auto handle = static_cast<AsyncHandle>(luaL_checkinteger(L, 1));
    ctx->CancelTimer(handle);
    return 0;
}

// --- await(resolve, reject) — async-to-coroutine bridge ---

enum { UV_CTX_PTR = 1, UV_HANDLE = 2, UV_DONE_TABLE = 3 };

int lua_await_resolve(lua_State* L) {
    // Check shared done flag
    lua_rawgeti(L, lua_upvalueindex(UV_DONE_TABLE), 1);
    bool done = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (done) return 0;

    // Set done
    lua_pushboolean(L, 1);
    lua_rawseti(L, lua_upvalueindex(UV_DONE_TABLE), 1);

    auto* ctx = static_cast<LuaContext*>(lua_touserdata(L, lua_upvalueindex(UV_CTX_PTR)));
    auto handle = static_cast<AsyncHandle>(lua_tointeger(L, lua_upvalueindex(UV_HANDLE)));

    int nargs = lua_gettop(L);
    auto values = LuaContext::PeekValues(L, nargs);
    ctx->PushResume(handle, std::move(values));
    return 0;
}

int lua_await_reject(lua_State* L) {
    // Check shared done flag
    lua_rawgeti(L, lua_upvalueindex(UV_DONE_TABLE), 1);
    bool done = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (done) return 0;

    // Set done
    lua_pushboolean(L, 1);
    lua_rawseti(L, lua_upvalueindex(UV_DONE_TABLE), 1);

    auto* ctx = static_cast<LuaContext*>(lua_touserdata(L, lua_upvalueindex(UV_CTX_PTR)));
    auto handle = static_cast<AsyncHandle>(lua_tointeger(L, lua_upvalueindex(UV_HANDLE)));

    // Resume with: nil, error_message
    std::vector<LuaValue> args;
    args.push_back(nullptr);
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        size_t len;
        const char* s = lua_tolstring(L, 1, &len);
        args.push_back(std::string(s, len));
    } else {
        args.push_back(std::string("rejected"));
    }
    ctx->PushResume(handle, std::move(args));
    return 0;
}

int lua_await(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    auto ctx = LuaContext::FromLuaState(L);

    // 1. Pre-yield: register coroutine in pending_
    auto handle = ctx->PreYield(L);

    // 2. Create shared done table (used by both resolve and reject)
    lua_newtable(L);  // [fn, done_table]

    // 3. Create resolve closure
    lua_pushlightuserdata(L, ctx.get());     // ctx ptr
    lua_pushinteger(L, handle);              // handle
    lua_pushvalue(L, 2);                     // shared done table
    lua_pushcclosure(L, lua_await_resolve, 3);
    // Stack: [fn(1), done_table(2), resolve(3)]

    // 4. Create reject closure
    lua_pushlightuserdata(L, ctx.get());     // ctx ptr
    lua_pushinteger(L, handle);              // handle
    lua_pushvalue(L, 2);                     // shared done table
    lua_pushcclosure(L, lua_await_reject, 3);
    // Stack: [fn(1), done_table(2), resolve(3), reject(4)]

    // 5. Submit fn(resolve, reject) as a scheduled task
    //    fn runs in its own coroutine via lua_resume, so it can yield freely
    lua_pushvalue(L, 1);   // fn
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushvalue(L, 3);   // resolve
    int resolve_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_pushvalue(L, 4);   // reject
    int reject_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    async_simple::Promise<ScriptResult> promise;
    ctx->PushTask({CallRef{fn_ref,
        {LuaRef{resolve_ref, LUA_TFUNCTION}, LuaRef{reject_ref, LUA_TFUNCTION}},
        true}, std::move(promise)});

    // 6. Yield the caller coroutine — resumes when resolve/reject is called
    return LuaContext::Yield(L);
}
}  // namespace

// --- LuaContext implementation ---

LuaContext::LuaContext(lua_State* main_L) : main_L_(main_L) {}

LuaContext::~LuaContext() = default;

// --- Extraspace ---

void LuaContext::SetExtraspace(lua_State* L, LuaContext* ctx) {
    std::memcpy(lua_getextraspace(L), &ctx, sizeof(LuaContext*));
}

LuaContext::Ptr LuaContext::FromLuaState(lua_State* L) {
    auto** ctx_ptr = reinterpret_cast<LuaContext**>(lua_getextraspace(L));
    return ctx_ptr && *ctx_ptr ? (*ctx_ptr)->shared_from_this() : nullptr;
}

// --- Configuration accessors ---

std::optional<lua_CFunction> LuaContext::find_c_module(const std::string& name) const {
    auto it = c_modules_.find(name);
    return it != c_modules_.end() ? std::optional(it->second) : std::nullopt;
}

std::shared_ptr<LuaLibrary> LuaContext::find_library(const std::string& name) const {
    auto it = libraries_.find(name);
    return it != libraries_.end() ? it->second : nullptr;
}

// --- Setup ---

void LuaContext::Setup(lua_State* main_L) {
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

void LuaContext::SetupBuiltins(lua_State* main_L) {
    lua_pushcfunction(main_L, lua_now);
    lua_setglobal(main_L, "now");

    lua_pushcfunction(main_L, lua_sleep);
    lua_setglobal(main_L, "sleep");

    lua_pushcfunction(main_L, lua_set_timeout);
    lua_setglobal(main_L, "setTimeout");

    lua_pushcfunction(main_L, lua_clear_timeout);
    lua_setglobal(main_L, "clearTimeout");

    lua_pushcfunction(main_L, lua_await);
    lua_setglobal(main_L, "await");
}

void LuaContext::SetupCustomRequire(lua_State* main_L) {
    if (!code_provider_ && c_modules_.empty() && libraries_.empty()) return;

    // Clear package.searchers (our custom require handles everything)
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

// --- Thread-safe submission ---

void LuaContext::PushTask(TaskRequest task) {
    bool should_reject = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            should_reject = true;
        } else {
            work_queue_.push(std::move(task));
        }
    }
    if (should_reject) {
        task.promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
        return;
    }
    cv_.notify_one();
}

void LuaContext::PushResume(AsyncHandle handle, std::vector<LuaValue> args) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutting_down_) {
            work_queue_.push(ResumeRequest{handle, std::move(args)});
            queued = true;
        }
    }
    if (queued) {
        cv_.notify_one();
    }
}

void LuaContext::PushRelease(std::vector<int> refs) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutting_down_) {
            work_queue_.push(std::move(refs));
            queued = true;
        }
    }
    if (queued) {
        cv_.notify_one();
    }
}

void LuaContext::PushRequireRun(AsyncHandle handle, std::string source, std::string module_name) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutting_down_) {
            work_queue_.push(RequireRun{std::move(source), std::move(module_name), handle});
            queued = true;
        }
    }
    if (queued) {
        cv_.notify_one();
    }
}

void LuaContext::PushLoadFileRun(AsyncHandle handle, std::string source, std::string filename) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutting_down_) {
            work_queue_.push(LoadFileRun{std::move(source), std::move(filename), handle});
            queued = true;
        }
    }
    if (queued) {
        cv_.notify_one();
    }
}

void LuaContext::CallLuaFunction(int fn_ref, std::vector<LuaValue> args) {
    async_simple::Promise<ScriptResult> promise;
    PushTask({CallRef{fn_ref, std::move(args), false}, std::move(promise)});
}

// --- Event loop processing ---

void LuaContext::ProcessExpiredTimers() {
    int64_t now_ms = NowMs();

    std::vector<TimerEntry> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!timer_queue_.empty() && timer_queue_.begin()->first <= now_ms) {
            expired.push_back(std::move(timer_queue_.begin()->second));
            timer_queue_.erase(timer_queue_.begin());
        }
    }

    for (auto& entry : expired) {
        if (entry.type == TimerType::kSleep) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutting_down_) {
                work_queue_.push(ResumeRequest{entry.handle, {}});
            }
        } else {
            async_simple::Promise<ScriptResult> promise;  // unused, fire-and-forget
            std::lock_guard<std::mutex> lock(mutex_);
            work_queue_.push(TaskRequest{CallRef{entry.fn_ref, {}, true}, std::move(promise)});
        }
    }
}

bool LuaContext::DrainOneResume() {
    std::optional<ResumeRequest> req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (work_queue_.empty()) return false;
        if (!std::holds_alternative<ResumeRequest>(work_queue_.front())) return false;
        req = std::move(std::get<ResumeRequest>(work_queue_.front()));
        work_queue_.pop();
    }
    DoResume(req->handle, std::move(req->args));
    return true;
}

bool LuaContext::DrainOneRelease() {
    std::optional<ReleaseRequest> refs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (work_queue_.empty()) return false;
        if (!std::holds_alternative<ReleaseRequest>(work_queue_.front())) return false;
        refs = std::move(std::get<ReleaseRequest>(work_queue_.front()));
        work_queue_.pop();
    }
    for (int ref : *refs) {
        luaL_unref(main_L_, LUA_REGISTRYINDEX, ref);
    }
    return true;
}

bool LuaContext::DrainOneWork() {
    std::optional<WorkItem> item;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (work_queue_.empty()) return false;
        item = std::move(work_queue_.front());
        work_queue_.pop();
    }

    return std::visit(overloaded{
                          [&](ResumeRequest& resume) {
                              DoResume(resume.handle, std::move(resume.args));
                              return true;
                          },
                          [&](ReleaseRequest& refs) {
                              for (int ref : refs) {
                                  luaL_unref(main_L_, LUA_REGISTRYINDEX, ref);
                              }
                              return true;
                          },
                          [&](TaskRequest& task) {
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
                                                     spdlog::error("LuaContext: {}", err ? err : "unknown error");
                                                     lua_pop(co, 1);
                                                     ReleaseCo(co);
                                                     ScriptResult result;
                                                     result.status = load_result;
                                                     result.error = err ? err : "unknown error";
                                                     task.promise.setValue(std::move(result));
                                                     co = nullptr;  // signal: no resume needed
                                                 }
                                                 // nargs stays 0 — loaded chunk takes no args
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

                              if (!co) return true;  // load error, already handled

                              int nresults = 0;
                              int status = lua_resume(co, main_L_, nargs, &nresults);

                              script_promises_[co] = std::move(task.promise);

                              if (status != LUA_YIELD) {
                                  MaybeRecycleCo(co, status, nresults);
                              }
                              return true;
                          },
                          [&](RequireRun& req) {
                              lua_State* co = AcquireCo();

                              std::string chunkname = "@" + req.module_name;
                              int load_status = luaL_loadbuffer(co, req.source.c_str(),
                                                               req.source.size(), chunkname.c_str());
                              if (load_status != LUA_OK) {
                                  const char* err = lua_tostring(co, -1);
                                  std::string errmsg = err ? err : "unknown error";
                                  spdlog::error("RequireRun: {}", errmsg);
                                  lua_pop(co, 1);
                                  ReleaseCo(co);
                                  PushResume(req.caller_handle, {LuaValue{nullptr}, LuaValue{std::move(errmsg)}});
                                  return true;
                              }

                              lua_pushstring(co, req.module_name.c_str());  // arg for chunk
                              int nresults = 0;
                              int status = lua_resume(co, main_L_, 1, &nresults);

                              if (status == LUA_OK) {
                                  // Chunk completed synchronously
                                  auto values = PeekValues(co, nresults);
                                  CacheModuleValues(main_L_, req.module_name.c_str(), values);

                                  PushResume(req.caller_handle, std::move(values));
                                  ReleaseCo(co);
                              } else if (status == LUA_YIELD) {
                                  // Chunk yielded (nested require) — set callback for when it finishes
                                  auto handle = req.caller_handle;
                                  auto mod_name = req.module_name;
                                  auto self = shared_from_this();
                                  SetCoCompleteCallback(co,
                                      [self, handle, mod_name](ScriptResult result) {
                                          if (result.status == LUA_OK) {
                                              auto& vals = result.values;
                                              CacheModuleValues(self->main_state(),
                                                                mod_name.c_str(), vals);
                                              self->PushResume(handle, std::move(vals));
                                          } else {
                                              auto err = result.error.empty()
                                                  ? "unknown error" : std::move(result.error);
                                              self->PushResume(handle,
                                                  {LuaValue{nullptr}, LuaValue{std::move(err)}});
                                          }
                                      });
                              } else {
                                  // Error
                                  const char* err = lua_tostring(co, -1);
                                  std::string errmsg = err ? err : "unknown error";
                                  spdlog::error("RequireRun: {}", errmsg);
                                  lua_pop(co, 1);
                                  PushResume(req.caller_handle,
                                      {LuaValue{nullptr}, LuaValue{std::move(errmsg)}});
                                  ReleaseCo(co);
                              }
                              return true;
                          },
                          [&](LoadFileRun& req) {
                              lua_State* co = AcquireCo();

                              std::string chunkname = "@" + req.filename;
                              int load_status = luaL_loadbuffer(co, req.source.c_str(),
                                                               req.source.size(), chunkname.c_str());
                              if (load_status != LUA_OK) {
                                  const char* err = lua_tostring(co, -1);
                                  spdlog::error("LoadFileRun: {}", err ? err : "unknown error");
                                  lua_pop(co, 1);
                                  ReleaseCo(co);
                                  PushResume(req.caller_handle, {LuaValue{nullptr}});
                                  return true;
                              }

                              // Chunk loaded successfully — return it to caller as LuaRef
                              lua_pushvalue(co, -1);  // copy chunk to keep ref after pop
                              int ref = luaL_ref(co, LUA_REGISTRYINDEX);
                              int type = lua_type(co, -1);
                              PushResume(req.caller_handle, {LuaRef{ref, type}});
                              ReleaseCo(co);
                              return true;
                          }},
                      *item);
}

// --- Timer management ---

void LuaContext::AddSleepTimer(int64_t deadline_ms, AsyncHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    timer_queue_.emplace(deadline_ms, TimerEntry{TimerType::kSleep, nullptr, LUA_NOREF, handle});
}

AsyncHandle LuaContext::AddTimeoutTimer(int64_t deadline_ms, int fn_ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto handle = next_handle_++;
    timer_queue_.emplace(deadline_ms, TimerEntry{TimerType::kSetTimeout, nullptr, fn_ref, handle});
    return handle;
}

void LuaContext::CancelTimer(AsyncHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = timer_queue_.begin(); it != timer_queue_.end(); ++it) {
        if (it->second.handle == handle) {
            if (it->second.fn_ref != LUA_NOREF) {
                luaL_unref(main_L_, LUA_REGISTRYINDEX, it->second.fn_ref);
            }
            timer_queue_.erase(it);
            return;
        }
    }
}

// --- Yield support ---

AsyncHandle LuaContext::PreYield(lua_State* co) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto handle = next_handle_++;
    pending_[handle] = {co};
    return handle;
}

int LuaContext::Yield(lua_State* L) {
    return lua_yield(L, 0);
}

// --- Wait/signal queries (caller must hold mutex_) ---

bool LuaContext::HasWork() const {
    return !work_queue_.empty();
}

std::optional<int64_t> LuaContext::NextTimerDeadline() const {
    if (timer_queue_.empty()) return std::nullopt;
    return timer_queue_.begin()->first;
}

// --- Value marshalling ---

std::vector<LuaValue> LuaContext::PeekValues(lua_State* L, int nresults) {
    std::vector<LuaValue> result;
    result.reserve(nresults);
    for (int i = -nresults; i < 0; ++i) {
        int t = lua_type(L, i);
        if (t == LUA_TNIL) {
            result.push_back(nullptr);
        } else if (t == LUA_TBOOLEAN) {
            result.push_back(static_cast<bool>(lua_toboolean(L, i)));
        } else if (t == LUA_TNUMBER) {
            if (lua_isinteger(L, i)) {
                result.push_back(static_cast<int64_t>(lua_tointeger(L, i)));
            } else {
                result.push_back(static_cast<double>(lua_tonumber(L, i)));
            }
        } else if (t == LUA_TSTRING) {
            size_t len;
            const char* s = lua_tolstring(L, i, &len);
            result.push_back(std::string(s, len));
        } else {
            lua_pushvalue(L, i);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            result.push_back(LuaRef{ref, t});
        }
    }
    return result;
}

void LuaContext::PushValues(lua_State* L, const std::vector<LuaValue>& values) {
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
                    lua_rawgeti(L, LUA_REGISTRYINDEX, val.ref);
                }
            },
            v);
    }
}

// --- Shutdown ---

void LuaContext::Shutdown() {
    std::queue<WorkItem> pending_work;
    decltype(script_promises_) pending_promises;
    decltype(timer_queue_) pending_timers;
    decltype(active_co_refs_) active_refs;
    decltype(co_pool_) pooled_refs;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        std::swap(work_queue_, pending_work);
        std::swap(script_promises_, pending_promises);
        std::swap(timer_queue_, pending_timers);
        std::swap(active_co_refs_, active_refs);
        std::swap(co_pool_, pooled_refs);
        pending_.clear();
        co_complete_callbacks_.clear();
    }

    for (auto& ext : extensions_) {
        ext->OnShutdown(main_L_);
    }
    for (auto& [name, lib] : libraries_) {
        lib->Close(main_L_);
    }
    while (!pending_work.empty()) {
        auto item = std::move(pending_work.front());
        pending_work.pop();
        std::visit(overloaded{
                       [&](TaskRequest& task) {
                           std::visit(overloaded{
                                          [&](const LoadScript&) {},
                                          [&](const CallRef& call) {
                                              luaL_unref(main_L_, LUA_REGISTRYINDEX, call.fn_ref);
                                          }},
                                      task.kind);
                           task.promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
                       },
                       [&](ResumeRequest&) {},
                       [&](ReleaseRequest& refs) {
                           for (int ref : refs) {
                               luaL_unref(main_L_, LUA_REGISTRYINDEX, ref);
                           }
                       },
                       [&](RequireRun& req) {
                           PushResume(req.caller_handle, {LuaValue{nullptr}});
                       },
                        [&](LoadFileRun& req) {
                            PushResume(req.caller_handle, {LuaValue{nullptr}});
                        }},
                    item);
    }

    for (auto& [co, promise] : pending_promises) {
        promise.setValue(ScriptResult{LUA_ERRRUN, {}, "runtime shutdown"});
    }
    for (auto& [deadline, entry] : pending_timers) {
        if (entry.fn_ref != LUA_NOREF) {
            luaL_unref(main_L_, LUA_REGISTRYINDEX, entry.fn_ref);
        }
    }
    for (auto& [co, ref] : active_refs) {
        SetExtraspace(co, nullptr);
        luaL_unref(main_L_, LUA_REGISTRYINDEX, ref);
    }
    for (auto& [co, ref] : pooled_refs) {
        SetExtraspace(co, nullptr);
        luaL_unref(main_L_, LUA_REGISTRYINDEX, ref);
    }
    SetExtraspace(main_L_, nullptr);
}

// --- Coroutine completion callback ---

void LuaContext::SetCoCompleteCallback(lua_State* co, CoCompleteCallback cb) {
    co_complete_callbacks_[co] = std::move(cb);
}

void LuaContext::RemoveCoCompleteCallback(lua_State* co) {
    co_complete_callbacks_.erase(co);
}

// --- Internal: coroutine pool ---

lua_State* LuaContext::AcquireCo() {
    if (!co_pool_.empty()) {
        auto [co, ref] = co_pool_.back();
        co_pool_.pop_back();
        active_co_refs_[co] = ref;
        return co;
    }

    lua_State* co = lua_newthread(main_L_);
    int ref = luaL_ref(main_L_, LUA_REGISTRYINDEX);
    SetExtraspace(co, this);
    active_co_refs_[co] = ref;
    return co;
}

void LuaContext::ReleaseCo(lua_State* co) {
    std::lock_guard<std::mutex> lock(mutex_);
    lua_settop(co, 0);
    auto it = active_co_refs_.find(co);
    if (it != active_co_refs_.end()) {
        co_pool_.push_back({co, it->second});
        active_co_refs_.erase(it);
    }
}

void LuaContext::MaybeRecycleCo(lua_State* co, int status, int nresults) {
    std::string error_msg;
    if (status != LUA_OK && status != LUA_YIELD) {
        const char* err = lua_tostring(co, -1);
        error_msg = err ? err : "unknown error";
        spdlog::error("LuaContext: {}", error_msg);
        lua_pop(co, 1);
    }
    if (status != LUA_YIELD) {
        std::vector<LuaValue> values;
        if (status == LUA_OK) {
            values = PeekValues(co, nresults);
        }

        auto it = script_promises_.find(co);
        if (it != script_promises_.end()) {
            ScriptResult result;
            result.status = status;
            result.values = values;
            result.error = error_msg;
            it->second.setValue(std::move(result));
            script_promises_.erase(it);
        }

        auto cb_it = co_complete_callbacks_.find(co);
        if (cb_it != co_complete_callbacks_.end()) {
            ScriptResult result;
            result.status = status;
            result.values = std::move(values);
            result.error = std::move(error_msg);
            cb_it->second(std::move(result));
            co_complete_callbacks_.erase(cb_it);
        }

        ReleaseCo(co);
    }
}

LuaContext::ResumeResult LuaContext::DoResume(AsyncHandle handle, std::vector<LuaValue> args) {
    lua_State* co = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(handle);
        if (it == pending_.end()) {
            spdlog::error("LuaContext::Resume: invalid handle {}", handle);
            return {nullptr, LUA_ERRRUN};
        }
        co = it->second.co;
        pending_.erase(it);
    }
    PushValues(co, args);
    int nresults = 0;
    int status = lua_resume(co, main_L_, static_cast<int>(args.size()), &nresults);
    spdlog::debug("DoResume handle={}: lua_resume status={}", handle, status);
    MaybeRecycleCo(co, status, nresults);
    return {co, status};
}
