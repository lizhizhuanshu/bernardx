#include "lua_runtime.h"

#include <chrono>

#include <spdlog/spdlog.h>

namespace {
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

LuaRuntime::LuaRuntime() : lua_(std::make_unique<sol::state>()) {
    context_ = std::make_shared<LuaContext>(lua_->lua_state());
    LuaContext::SetExtraspace(lua_->lua_state(), context_.get());
    lua_->open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                         sol::lib::math, sol::lib::package);
}

void LuaRuntime::Setup(sol::state& lua, const std::shared_ptr<CodeProvider>& code_provider,
                       const std::unordered_map<std::string, lua_CFunction>& c_modules,
                       const std::unordered_map<std::string, std::shared_ptr<LuaLibrary>>& libraries,
                       async_simple::Executor* executor,
                       const std::vector<std::shared_ptr<LuaExtension>>& extensions) {
    auto ctx = LuaContext::FromLuaState(lua.lua_state());

    ctx->SetCodeProvider(code_provider);
    ctx->SetExecutor(executor);
    ctx->SetCModules(c_modules);
    ctx->SetLibraries(libraries);
    ctx->SetExtensions(extensions);

    ctx->Setup(lua.lua_state());
}

void LuaRuntime::Start() {
    running_ = true;
    event_loop_thread_ = std::thread(&LuaRuntime::EventLoop, this);
}

void LuaRuntime::Stop() {
    running_.store(false, std::memory_order_release);
    context_->cv().notify_all();
    if (event_loop_thread_.joinable()) {
        event_loop_thread_.join();
    }
}

LuaRuntime::~LuaRuntime() {
    Stop();
    context_->Shutdown();
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::RunScript(const std::string& script) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    context_->PushTask({LoadScript{script, "=script"}, std::move(promise)});
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::RunFile(const std::string& filename) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    context_->PushTask({LoadScript{"", filename}, std::move(promise)});
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<ScriptResult> LuaRuntime::CallFunction(int fn_ref, std::vector<LuaValue> args) {
    async_simple::Promise<ScriptResult> promise;
    auto future = promise.getFuture();
    context_->PushTask({CallRef{fn_ref, std::move(args), false}, std::move(promise)});
    co_return co_await std::move(future);
}

void LuaRuntime::WaitOrTimeout() {
    std::unique_lock<std::mutex> lock(context_->mutex());
    auto deadline = context_->NextTimerDeadline();
    if (deadline.has_value()) {
        int64_t wait_ms = std::max<int64_t>(0, *deadline - NowMs());
        context_->cv().wait_for(lock, std::chrono::milliseconds(wait_ms), [this] {
            return !running_.load(std::memory_order_acquire) || context_->HasWork();
        });
    } else {
        context_->cv().wait(lock, [this] {
            return !running_.load(std::memory_order_acquire) || context_->HasWork();
        });
    }
}

void LuaRuntime::EventLoop() {
    while (running_.load(std::memory_order_acquire)) {
        context_->ProcessExpiredTimers();
        while (context_->DrainOneWork()) {
        }
        WaitOrTimeout();
    }
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

LuaRuntime::Ptr LuaRuntime::Builder::Create() {
    auto rt = std::shared_ptr<LuaRuntime>(new LuaRuntime());
    LuaRuntime::Setup(rt->lua(), code_provider_, c_modules_, libraries_, executor_, extensions_);
    rt->Start();
    return rt;
}
