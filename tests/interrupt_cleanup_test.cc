// Post-interrupt cleanup semantics of LuaRuntime.
//
// Hosts run addCleanup functions after a run ends (success, error or
// interrupt), each in its own coroutine via CallAsync so async ops keep
// working. These tests pin the runtime guarantees that relies on:
//
//   - a coroutine parked on an async op AFTER the interrupt completion pass
//     still gets its resume (PushResume is no longer interrupt-gated; the
//     pass's pending_ clear already neutralizes stale completions)
//   - new PushTask work stays rejected after an interrupt (sticky flag,
//     see InterruptedRuntimeRejectsNewTasks in lua_runtime_test.cc) until
//     the host calls ClearInterrupt — which is what lets a kept runtime
//     (engine kAutoRecycle off) run again
//   - interrupting DURING such a cleanup coroutine completes it with
//     "interrupted" instead of stranding it

#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

namespace {

// Engine-shaped helper: run the fn_ref in its own coroutine on the runtime
// executor, exactly how BernardXEngine::RunCode drives addCleanup functions.
ScriptResult RunCleanupRef(const LuaRuntime::Ptr& rt, int fn_ref) {
    return async_simple::coro::syncAwait(
        [](LuaRuntime::Ptr rt, int fn_ref)
                -> async_simple::coro::Lazy<ScriptResult> {
            co_return co_await rt->CallAsync(fn_ref);
        }(rt, fn_ref), rt->executor());
}

// set_function with a capturing lambda does not bind as a raw C function
// (sol2 tries to marshal lua_State* as an argument) — use a plain function
// plus a file-scope pointer instead.
static std::atomic<bool>* g_cleanup_entered = nullptr;
static int l_mark_cleanup_entered(lua_State*) {
    if (g_cleanup_entered) g_cleanup_entered->store(true);
    return 0;
}

class InterruptCleanupTest : public ::testing::Test {
protected:
    void SetUp() override {
        rt = LuaRuntime::Builder().Create();

        // An async op that parks forever: a run can only end via Interrupt.
        rt->lua().set_function("park_forever", [](lua_State* L) -> int {
            auto ctx = LuaRuntime::FromLuaState(L);
            (void)ctx->PreYield(L);  // no PushResume will ever come
            return ctx->Yield(L);
        });

        // Input-manager shaped async op: work on another thread, resumed via
        // PushResume (this is the path the old interrupt gate dropped).
        rt->lua().set_function("resume_later", [](lua_State* L) -> int {
            auto ctx = LuaRuntime::FromLuaState(L);
            auto handle = ctx->PreYield(L);
            std::thread([ctx, handle]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ctx->PushResume(handle);
            }).detach();
            return ctx->Yield(L);
        });
    }

    void TearDown() override {
        g_cleanup_entered = nullptr;
        ::alarm(0);  // cancel any watchdog armed by a test
    }

    LuaRuntime::Ptr rt;

    // arm a watchdog: a dropped resume used to hang the awaiting host forever
    void ArmHangWatchdog(int seconds = 30) { ::alarm(seconds); }

    int RefGlobal(const char* name) {
        lua_State* L = rt->main_state();
        lua_getglobal(L, name);
        return luaL_ref(L, LUA_REGISTRYINDEX);
    }

    // Interrupt a run parked on park_forever and await its "interrupted"
    // result. Returns false on an unexpected status.
    bool InterruptParkedRun() {
        std::thread interrupter([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            rt->Interrupt();
        });
        auto r = AWAIT(rt->RunScript("park_forever()"));
        interrupter.join();
        return r.status == LUA_ERRRUN && r.error == "interrupted";
    }
};

TEST_F(InterruptCleanupTest, AsyncOpsWorkInCoroutineAfterInterrupt) {
    ArmHangWatchdog();
    ASSERT_EQ(AWAIT(rt->RunScript(
        "function __cleanup() resume_later() sleep(60) __ran = true end"))
        .status, LUA_OK);
    const int ref = RefGlobal("__cleanup");

    ASSERT_TRUE(InterruptParkedRun())
        << "setup: parked run should end with 'interrupted'";

    // The cleanup coroutine must complete: both the PushResume-based op and
    // the timer-based sleep have to park AND come back after the interrupt
    // pass (>= 50ms other-thread op + 60ms sleep).
    auto start = std::chrono::steady_clock::now();
    auto cr = RunCleanupRef(rt, ref);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_EQ(cr.status, LUA_OK);
    EXPECT_GE(elapsed.count(), 110);

    lua_State* L = rt->main_state();
    lua_getglobal(L, "__ran");
    EXPECT_TRUE(lua_toboolean(L, -1)) << "cleanup body did not finish";
    lua_pop(L, 1);
}

TEST_F(InterruptCleanupTest, InterruptDuringCleanupCorCompletesIt) {
    ArmHangWatchdog();
    std::atomic<bool> entered{false};
    g_cleanup_entered = &entered;
    rt->lua().set_function("mark_entered", &l_mark_cleanup_entered);
    ASSERT_EQ(AWAIT(rt->RunScript(
        "function __cleanup() mark_entered() park_forever() end")).status,
        LUA_OK);
    const int ref = RefGlobal("__cleanup");

    ASSERT_TRUE(InterruptParkedRun())
        << "setup: parked run should end with 'interrupted'";

    // Engine flow: clear the sticky flag before cleanup so a fresh Interrupt
    // during cleanup actually delivers (legacy waiter_->Clean(kInterrupt)).
    async_simple::coro::syncAwait(rt->ClearInterrupt(), rt->executor());

    // The cleanup parks on park_forever; a second Interrupt must complete it
    // with "interrupted" rather than stranding the awaiting host. Without the
    // clear, Interrupt()'s exchange early-returns and this hangs.
    std::thread interrupter([this, &entered]() {
        while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        rt->Interrupt();
    });
    auto cr = RunCleanupRef(rt, ref);
    interrupter.join();
    EXPECT_EQ(cr.status, LUA_ERRRUN);
    EXPECT_EQ(cr.error, "interrupted");
}

TEST_F(InterruptCleanupTest, ClearInterruptReenablesTaskSubmission) {
    ArmHangWatchdog();
    ASSERT_TRUE(InterruptParkedRun())
        << "setup: parked run should end with 'interrupted'";

    // Sticky semantics: new tasks stay rejected after an interrupt...
    auto rejected = AWAIT(rt->RunScript("return 1"));
    EXPECT_NE(rejected.status, LUA_OK);
    EXPECT_NE(rejected.error.find("shutdown"), std::string::npos);

    // ...until the host clears the flag via the executor (the kept-runtime /
    // kAutoRecycle-off reuse path in the engine).
    async_simple::coro::syncAwait(rt->ClearInterrupt(), rt->executor());

    auto ok = AWAIT(rt->RunScript("return 7"));
    EXPECT_EQ(ok.status, LUA_OK);
    ASSERT_EQ(ok.values.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(ok.values[0]), 7);
}

}  // namespace
