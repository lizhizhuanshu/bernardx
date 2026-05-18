#include "lua_runtime.h"
#include "lua_extension.h"
#include "lua_library.h"

#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <cerrno>
#include <csignal>
#include <map>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Helper to syncAwait a RunScript/RunFile Lazy
#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

// --- Test CodeProvider (async via async_simple) ---

class TestCodeProvider : public CodeProvider {
public:
    async_simple::coro::Lazy<std::optional<std::string>> LoadModule(const std::string& name) override {
        auto it = modules_.find(name);
        co_return it != modules_.end() ? std::optional(it->second) : std::nullopt;
    }
    async_simple::coro::Lazy<std::optional<std::string>> LoadFile(const std::string& path) override {
        auto it = files_.find(path);
        co_return it != files_.end() ? std::optional(it->second) : std::nullopt;
    }
    void set_module(const std::string& name, const std::string& source) {
        modules_[name] = source;
    }
    void set_file(const std::string& path, const std::string& source) {
        files_[path] = source;
    }
private:
    std::map<std::string, std::string> modules_;
    std::map<std::string, std::string> files_;
};

class LuaRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        rt = LuaRuntime::Builder().Create();
    }

    LuaRuntime::Ptr rt;
};

// --- RunScript ---

TEST_F(LuaRuntimeTest, RunScriptReturnsOkOnSuccess) {
    EXPECT_EQ(AWAIT(rt->RunScript("print('hello')")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsErrorOnSyntaxError) {
    auto r = AWAIT(rt->RunScript("if true"));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_FALSE(r.error.empty());
}

TEST_F(LuaRuntimeTest, RunScriptReturnsErrorOnRuntimeError) {
    auto r = AWAIT(rt->RunScript("error('boom')"));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_NE(r.error.find("boom"), std::string::npos);
}

TEST_F(LuaRuntimeTest, RunScriptCanReadGlobalSetFromC) {
    rt->lua()["x"] = 42;
    EXPECT_EQ(AWAIT(rt->RunScript("assert(x == 42)")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptCanCallCFunction) {
    rt->lua().set_function("double_it", [](int v) { return v * 2; });
    EXPECT_EQ(AWAIT(rt->RunScript("assert(double_it(5) == 10)")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsValues) {
    auto r = AWAIT(rt->RunScript("return 42, 'hello', 3.14"));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 3u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 42);
    EXPECT_EQ(std::get<std::string>(r.values[1]), "hello");
    EXPECT_DOUBLE_EQ(std::get<double>(r.values[2]), 3.14);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsNilValue) {
    auto r = AWAIT(rt->RunScript("return nil"));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
}

TEST_F(LuaRuntimeTest, RunScriptReturnsBoolValue) {
    auto r = AWAIT(rt->RunScript("return true, false"));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_FALSE(std::get<bool>(r.values[1]));
}

TEST_F(LuaRuntimeTest, RunScriptReturnsNoValues) {
    auto r = AWAIT(rt->RunScript("local x = 1"));
    EXPECT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(r.values.empty());
}


// --- Async yield/resume: single ---

TEST_F(LuaRuntimeTest, AsyncNoArgs) {
    rt->lua().set_function("async_noop", [](lua_State* L) -> int {
        auto ctx = LuaRuntime::FromLuaState(L);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle]() { ctx->PushResume(handle); }).detach();
        return ctx->Yield(L);
    });
    EXPECT_EQ(AWAIT(rt->RunScript("async_noop()")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsIntValue) {
    rt->lua().set_function("async_value", [](lua_State* L) -> int {
        auto ctx = LuaRuntime::FromLuaState(L);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle]() {
            ctx->PushResume(handle, {static_cast<int64_t>(99)});
        }).detach();
        return ctx->Yield(L);
    });
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local v = async_value()
        assert(v == 99, "expected 99 got " .. tostring(v))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsDoubleValue) {
    rt->lua().set_function("async_double", [](lua_State* L) -> int {
        auto ctx = LuaRuntime::FromLuaState(L);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle]() {
            ctx->PushResume(handle, {3.14});
        }).detach();
        return ctx->Yield(L);
    });
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local v = async_double()
        assert(math.abs(v - 3.14) < 0.001)
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsBoolValue) {
    rt->lua().set_function("async_bool", [](lua_State* L) -> int {
        auto ctx = LuaRuntime::FromLuaState(L);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle]() {
            ctx->PushResume(handle, {LuaValue{true}});
        }).detach();
        return ctx->Yield(L);
    });
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local v = async_bool()
        assert(v == true)
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsStringValue) {
    rt->lua().set_function("async_str", [](lua_State* L) -> int {
        auto ctx = LuaRuntime::FromLuaState(L);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle]() {
            ctx->PushResume(handle, {std::string("hello from C++")});
        }).detach();
        return ctx->Yield(L);
    });
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local v = async_str()
        assert(v == "hello from C++")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AsyncReturnsNilValue) {
    rt->lua().set_function("async_nil", [](lua_State* L) -> int {
        auto ctx = LuaRuntime::FromLuaState(L);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle]() {
            ctx->PushResume(handle, {LuaValue{nullptr}});
        }).detach();
        return ctx->Yield(L);
    });
    EXPECT_EQ(AWAIT(rt->RunScript("local v = async_nil(); assert(v == nil)")).status, LUA_OK);
}

// --- Multiple sequential async calls in one script ---

TEST_F(LuaRuntimeTest, SequentialAsyncCalls) {
    rt->lua().set_function("async_add", [](lua_State* L) -> int {
        int a = static_cast<int>(luaL_checkinteger(L, 1));
        int b = static_cast<int>(luaL_checkinteger(L, 2));
        auto ctx = LuaRuntime::FromLuaState(L);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle, a, b]() {
            ctx->PushResume(handle, {static_cast<int64_t>(a + b)});
        }).detach();
        return ctx->Yield(L);
    });

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = async_add(1, 2)
        local b = async_add(a, 3)
        local c = async_add(b, 4)
        assert(c == 10, "expected 10 got " .. tostring(c))
    )")).status, LUA_OK);
}

// --- Concurrent async calls on separate LuaRuntime instances ---

TEST_F(LuaRuntimeTest, ConcurrentRuntimes) {
    auto make_rt = []() {
        auto r = LuaRuntime::Builder().Create();
        r->lua().set_function("async_id", [](lua_State* L) -> int {
            int id = static_cast<int>(luaL_checkinteger(L, 1));
            auto ctx = LuaRuntime::FromLuaState(L);
            auto handle = ctx->PreYield(L);
            std::thread([ctx, handle, id]() {
                ctx->PushResume(handle, {static_cast<int64_t>(id)});
            }).detach();
            return ctx->Yield(L);
        });
        return r;
    };

    auto rt1 = make_rt();
    auto rt2 = make_rt();

    int r1 = LUA_ERRRUN, r2 = LUA_ERRRUN;
    std::thread t1([&r1, rt1]() {
        r1 = AWAIT(rt1->RunScript(R"(
            local v = async_id(1)
            assert(v == 1)
        )")).status;
    });

    std::thread t2([&r2, rt2]() {
        r2 = AWAIT(rt2->RunScript(R"(
            local v = async_id(2)
            assert(v == 2)
        )")).status;
    });

    t1.join();
    t2.join();

    EXPECT_EQ(r1, LUA_OK);
    EXPECT_EQ(r2, LUA_OK);
}

// --- Built-in: now, sleep, setTimeout ---

TEST_F(LuaRuntimeTest, NowReturnsMilliseconds) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local t = now()
        assert(type(t) == "number")
        assert(t > 0)
        -- sleep 50ms and check that time advanced
        sleep(50)
        assert(now() - t >= 50, "elapsed too short: " .. tostring(now() - t))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, SleepBlocksForDuration) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local t = now()
        sleep(100)
        local elapsed = now() - t
        assert(elapsed >= 80, "slept too short: " .. tostring(elapsed) .. "ms")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, SetTimeoutCallsCallback) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local done = false
        local t = now()
        setTimeout(80, function()
            done = true
        end)
        sleep(150)
        assert(done, "callback not called after timeout")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, SetTimeoutWithMultipleCallbacks) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local order = {}
        setTimeout(100, function() order[#order + 1] = "a" end)
        setTimeout(50, function()  order[#order + 1] = "b" end)
        setTimeout(25, function()  order[#order + 1] = "c" end)
        sleep(200)
        assert(order[1] == "c", "expected c first, got " .. tostring(order[1]))
        assert(order[2] == "b", "expected b second, got " .. tostring(order[2]))
        assert(order[3] == "a", "expected a third, got " .. tostring(order[3]))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, SetTimeoutReturnsHandle) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local t = setTimeout(100, function() end)
        assert(type(t) == "number", "expected number handle, got " .. type(t))
        assert(t > 0, "expected positive handle")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, ClearTimeoutPreventsCallback) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local done = false
        local t = setTimeout(50, function() done = true end)
        clearTimeout(t)
        sleep(100)
        assert(done == false, "callback should not fire after clearTimeout")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, ClearTimeoutWithMultipleTimers) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local results = {}
        local t1 = setTimeout(100, function() results[#results + 1] = "a" end)
        local t2 = setTimeout(50, function()  results[#results + 1] = "b" end)
        local t3 = setTimeout(25, function()  results[#results + 1] = "c" end)
        clearTimeout(t1)  -- cancel the 100ms one
        sleep(200)
        assert(#results == 2, "expected 2 callbacks, got " .. tostring(#results))
        assert(results[1] == "c", "expected c first")
        assert(results[2] == "b", "expected b second")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, ClearTimeoutOnFiredTimerIsHarmless) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local done = false
        local t = setTimeout(50, function() done = true end)
        sleep(100)
        assert(done == true, "callback should have fired")
        clearTimeout(t)  -- no-op, should not crash
    )")).status, LUA_OK);
}

// --- await() tests ---

TEST_F(LuaRuntimeTest, AwaitResolveWithValue) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local value = await(function(resolve)
            setTimeout(50, function()
                resolve(42)
            end)
        end)
        assert(value == 42, "expected 42, got " .. tostring(value))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitResolveWithString) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local msg = await(function(resolve)
            setTimeout(50, function()
                resolve("hello")
            end)
        end)
        assert(msg == "hello", "expected hello, got " .. tostring(msg))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitRejectReturnsNilAndError) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local result, err = await(function(resolve, reject)
            setTimeout(50, function()
                reject("something went wrong")
            end)
        end)
        assert(result == nil, "expected nil result on reject")
        assert(err == "something went wrong", "expected error message, got " .. tostring(err))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitResolveCalledTwiceIsNoOp) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local value = await(function(resolve)
            setTimeout(50, function()
                resolve(1)
                resolve(2)  -- should be ignored
            end)
        end)
        assert(value == 1, "expected 1, got " .. tostring(value))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitResolveAndRejectRace) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local value, err = await(function(resolve, reject)
            setTimeout(50, function()
                resolve("ok")
                reject("fail")  -- should be ignored
            end)
        end)
        assert(value == "ok", "expected ok, got " .. tostring(value))
        assert(err == nil, "expected nil error")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitSequentialCalls) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = await(function(resolve)
            setTimeout(30, function() resolve(10) end)
        end)
        local b = await(function(resolve)
            setTimeout(30, function() resolve(a + 5) end)
        end)
        local c = await(function(resolve)
            setTimeout(30, function() resolve(b * 2) end)
        end)
        assert(c == 30, "expected 30, got " .. tostring(c))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitFnErrorKillsTask) {
    // fn errors are not auto-caught; fn should use pcall to handle errors
    auto result = AWAIT(rt->RunScript(R"(
        local result, err = await(function(resolve, reject)
            local ok, e = pcall(function() error("boom") end)
            if not ok then reject(e) end
        end)
        assert(result == nil, "expected nil result")
        assert(err ~= nil, "expected error message")
        assert(string.find(err, "boom"), "error should contain 'boom', got: " .. tostring(err))
    )"));
    EXPECT_EQ(result.status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitWithYieldInsideFn) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local value = await(function(resolve)
            sleep(50)
            resolve(99)
        end)
        assert(value == 99, "expected 99, got " .. tostring(value))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitWithMultipleYieldsInsideFn) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local value = await(function(resolve)
            sleep(30)
            sleep(30)
            resolve("delayed")
        end)
        assert(value == "delayed", "expected 'delayed', got " .. tostring(value))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitRejectWithNoMessage) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local result, err = await(function(resolve, reject)
            setTimeout(50, function()
                reject()
            end)
        end)
        assert(result == nil, "expected nil result")
        assert(err == "rejected", "expected 'rejected', got " .. tostring(err))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, AwaitSynchronousResolve) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local value = await(function(resolve)
            resolve(99)  -- resolve synchronously before yield
        end)
        assert(value == 99, "expected 99, got " .. tostring(value))
    )")).status, LUA_OK);
}

// --- Custom require and loadfile ---

class LuaRuntimeWithProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto p = std::make_unique<TestCodeProvider>();
        provider = p.get();
        rt = LuaRuntime::Builder()
            .WithCodeProvider(std::move(p))
            .Create();
    }
    TestCodeProvider* provider = nullptr;
    LuaRuntime::Ptr rt;
};

TEST_F(LuaRuntimeWithProviderTest, RequireLuaModuleViaCodeProvider) {
    provider->set_module("greet", "return { hello = function() return 'hi' end }");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local greet = require("greet")
        assert(greet.hello() == "hi")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, RequireModuleNotFound) {
    auto r = AWAIT(rt->RunScript(R"(
        require("nonexistent_module")
    )"));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_TRUE(r.error.find("not found") != std::string::npos)
        << "expected 'not found', got: " << r.error;
}

TEST_F(LuaRuntimeWithProviderTest, RequireModuleSyntaxError) {
    provider->set_module("bad_syntax", "local x = ");
    auto r = AWAIT(rt->RunScript(R"(
        require("bad_syntax")
    )"));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_TRUE(r.error.find("error loading module") != std::string::npos)
        << "expected 'error loading module', got: " << r.error;
}

TEST_F(LuaRuntimeWithProviderTest, RequireModuleRuntimeError) {
    provider->set_module("bad_runtime", R"(
        error("something went wrong inside module")
    )");
    auto r = AWAIT(rt->RunScript(R"(
        require("bad_runtime")
    )"));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_TRUE(r.error.find("error loading module") != std::string::npos)
        << "expected 'error loading module', got: " << r.error;
    EXPECT_TRUE(r.error.find("something went wrong") != std::string::npos)
        << "expected error detail, got: " << r.error;
}

TEST_F(LuaRuntimeWithProviderTest, NestedRequireFiveLevelsDeep) {
    // Level 5 (leaf): no further require
    provider->set_module("level5", "return { value = 5 }");
    // Level 4: requires level5
    provider->set_module("level4", R"(
        local l5 = require("level5")
        return { value = 4 + l5.value }
    )");
    // Level 3: requires level4
    provider->set_module("level3", R"(
        local l4 = require("level4")
        return { value = 3 + l4.value }
    )");
    // Level 2: requires level3
    provider->set_module("level2", R"(
        local l3 = require("level3")
        return { value = 2 + l3.value }
    )");
    // Level 1: requires level2
    provider->set_module("level1", R"(
        local l2 = require("level2")
        return { value = 1 + l2.value }
    )");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local l1 = require("level1")
        assert(l1.value == 15, "expected 15 got " .. tostring(l1.value))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, RequireCaching) {
    provider->set_module("counter", R"(
        _G._load_count = (_G._load_count or 0) + 1
        return { count = _G._load_count }
    )");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = require("counter")
        local b = require("counter")
        assert(a.count == 1, "expected 1 got " .. tostring(a.count))
        assert(a == b, "expected same table on second require")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileRelativePath) {
    provider->set_file("test.lua", "return 42 + 1");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local fn = loadfile("test.lua")
        assert(type(fn) == "function")
        local result = fn()
        assert(result == 43)
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileNestedRequire) {
    // loadfile loads a chunk that calls require, which loads another module
    provider->set_module("helper", "return { add = function(a, b) return a + b end }");
    provider->set_file("calc.lua", R"(
        local h = require("helper")
        return h.add(10, 20)
    )");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local fn = loadfile("calc.lua")
        assert(type(fn) == "function")
        local result = fn()
        assert(result == 30, "expected 30 got " .. tostring(result))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileNestedLoadfile) {
    // loadfile loads a chunk that itself calls loadfile
    provider->set_file("inner.lua", "return 100");
    provider->set_file("outer.lua", R"(
        local fn = loadfile("inner.lua")
        return fn()
    )");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local fn = loadfile("outer.lua")
        local result = fn()
        assert(result == 100, "expected 100 got " .. tostring(result))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileRelativeNotFound) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local fn, err = loadfile("missing.lua")
        assert(fn == nil, "expected nil, got " .. tostring(fn))
        assert(err ~= nil, "expected error message")
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeWithProviderTest, LoadfileAbsolutePath) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local fn, err = loadfile("/nonexistent/path.lua")
        assert(fn == nil, "expected nil for nonexistent file")
        assert(err ~= nil, "expected error message")
    )")).status, LUA_OK);
}

class LuaRuntimeRegisterTest : public ::testing::Test {
protected:
    static int luaopen_testmath(lua_State* L) {
        lua_newtable(L);
        lua_pushcfunction(L, [](lua_State* L) -> int {
            int a = static_cast<int>(luaL_checkinteger(L, 1));
            int b = static_cast<int>(luaL_checkinteger(L, 2));
            lua_pushinteger(L, a * b);
            return 1;
        });
        lua_setfield(L, -2, "mul");
        return 1;
    }

    void SetUp() override {
        rt = LuaRuntime::Builder()
            .Register("testmath", luaopen_testmath)
            .Create();
    }

    LuaRuntime::Ptr rt;
};

TEST_F(LuaRuntimeRegisterTest, RequireCModule) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local m = require("testmath")
        assert(m.mul(3, 4) == 12)
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeRegisterTest, RequireCModuleCached) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = require("testmath")
        local b = require("testmath")
        assert(a == b, "expected same table")
    )")).status, LUA_OK);
}

class LuaRuntimeFluentBuilderTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto provider = std::make_unique<TestCodeProvider>();
        provider->set_module("util", "return { answer = 42 }");
        provider->set_module("helper", "return { greet = function() return 'hello' end }");
        rt = LuaRuntime::Builder()
            .Register("util", [](lua_State* L) -> int {
                lua_newtable(L);
                lua_pushstring(L, "from_c");
                lua_setfield(L, -2, "source");
                return 1;
            })
            .WithCodeProvider(std::move(provider))
            .Create();
    }
    LuaRuntime::Ptr rt;
};

TEST_F(LuaRuntimeFluentBuilderTest, FluentChainingWorks) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local u = require("util")
        assert(u.source == "from_c", "expected C module via preload")
        local h = require("helper")
        assert(h.greet() == "hello", "expected Lua module via CodeProvider")
    )")).status, LUA_OK);
}

// --- C++ side CallLuaFunction ---

TEST_F(LuaRuntimeTest, CallLuaFunctionFromCpp) {
    lua_State* main_L = rt->lua().lua_state();

    lua_pushlightuserdata(main_L, main_L);
    lua_pushcclosure(main_L, [](lua_State* L) -> int {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        auto ctx = LuaRuntime::FromLuaState(L);
        lua_pushvalue(L, 1);
        int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        std::thread([ctx, fn_ref]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            async_simple::Promise<ScriptResult> promise;
            ctx->PushTask({CallRef{fn_ref, {}, false}, std::move(promise)});
            ctx->PushRelease({fn_ref});
        }).detach();
        return 0;
    }, 1);
    lua_setglobal(main_L, "store_callback");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local done = false
        store_callback(function()
            done = true
        end)
        while not done do sleep(10) end
        assert(done)
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, CallLuaFunctionWithArgs) {
    lua_State* main_L = rt->lua().lua_state();

    lua_pushlightuserdata(main_L, main_L);
    lua_pushcclosure(main_L, [](lua_State* L) -> int {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        auto ctx = LuaRuntime::FromLuaState(L);
        lua_pushvalue(L, 1);
        int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

        std::thread([ctx, fn_ref]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            async_simple::Promise<ScriptResult> promise;
            ctx->PushTask({CallRef{fn_ref, {static_cast<int64_t>(42), std::string("hello")}, false}, std::move(promise)});
            ctx->PushRelease({fn_ref});
        }).detach();
        return 0;
    }, 1);
    lua_setglobal(main_L, "store_callback");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local got_a, got_b = nil, nil
        store_callback(function(a, b)
            got_a = a
            got_b = b
        end)
        while got_a == nil do sleep(10) end
        assert(got_a == 42, "expected 42 got " .. tostring(got_a))
        assert(got_b == "hello", "expected hello got " .. tostring(got_b))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsTableAsLuaRef) {
    auto r = AWAIT(rt->RunScript("return {1, 2, 3}"));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<LuaRef>(r.values[0]));
    auto& lr = std::get<LuaRef>(r.values[0]);
    EXPECT_EQ(lr->type, LUA_TTABLE);
    EXPECT_NE(lr->ref, LUA_NOREF);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsFunctionAsLuaRef) {
    auto r = AWAIT(rt->RunScript("return function() return 42 end"));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<LuaRef>(r.values[0]));
    auto& lr = std::get<LuaRef>(r.values[0]);
    EXPECT_EQ(lr->type, LUA_TFUNCTION);
    EXPECT_NE(lr->ref, LUA_NOREF);
}

TEST_F(LuaRuntimeTest, RunScriptReturnsMixedWithLuaRef) {
    auto r = AWAIT(rt->RunScript("return 42, {a=1}, 'hello', function() end"));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<int64_t>(r.values[0]));
    EXPECT_TRUE(std::holds_alternative<LuaRef>(r.values[1]));
    EXPECT_EQ(std::get<LuaRef>(r.values[1])->type, LUA_TTABLE);
    EXPECT_TRUE(std::holds_alternative<std::string>(r.values[2]));
    EXPECT_TRUE(std::holds_alternative<LuaRef>(r.values[3]));
    EXPECT_EQ(std::get<LuaRef>(r.values[3])->type, LUA_TFUNCTION);
}

TEST_F(LuaRuntimeTest, LuaRefRoundTripViaResume) {
    lua_State* main_L = rt->lua().lua_state();

    lua_pushlightuserdata(main_L, main_L);
    lua_pushcclosure(main_L, [](lua_State* L) -> int {
        luaL_checktype(L, 1, LUA_TTABLE);
        auto ctx = LuaRuntime::FromLuaState(L);
        lua_pushvalue(L, 1);
        int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle, fn_ref]() {
            ctx->PushResume(handle, {ctx->CreateRef(fn_ref, LUA_TTABLE)});
        }).detach();
        return ctx->Yield(L);
    }, 1);
    lua_setglobal(main_L, "async_echo_table");

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local t = {key = "value"}
        local result = async_echo_table(t)
        assert(result.key == "value", "expected 'value' got " .. tostring(result.key))
    )")).status, LUA_OK);
}

TEST_F(LuaRuntimeTest, ReleaseRefsBatch) {
    lua_State* main_L = rt->lua().lua_state();

    // All ref operations must run on the Lua thread to avoid races with main_L
    rt->lua().set_function("make_ref", [main_L]() -> int {
        lua_pushcfunction(main_L, [](lua_State*) -> int { return 0; });
        return luaL_ref(main_L, LUA_REGISTRYINDEX);
    });
    rt->lua().set_function("do_release", [this](sol::variadic_args va) {
        std::vector<int> refs;
        for (auto v : va) {
            refs.push_back(v.as<int>());
        }
        rt->PushRelease(std::move(refs));
    });

    // Create refs, release them, then verify slots are reused by new refs
    auto r = AWAIT(rt->RunScript(R"(
        local r1 = make_ref()
        local r2 = make_ref()
        local r3 = make_ref()
        do_release(r1, r2, r3)
        -- sleep yields; event loop processes release_queue_ before timer fires
        sleep(10)
        -- New refs should reuse the freed slots (Lua free list is LIFO)
        local r4 = make_ref()
        local r5 = make_ref()
        local r6 = make_ref()
        assert(r4 == r3, 'r4 should reuse r3 slot')
        assert(r5 == r2, 'r5 should reuse r2 slot')
        assert(r6 == r1, 'r6 should reuse r1 slot')
    )"));
    EXPECT_EQ(r.status, LUA_OK);
}

// --- CallFunction ---

TEST_F(LuaRuntimeTest, CallFunctionReturnsValues) {
    lua_State* main_L = rt->lua().lua_state();
    lua_pushcfunction(main_L, [](lua_State* L) -> int {
        int a = static_cast<int>(luaL_checkinteger(L, 1));
        int b = static_cast<int>(luaL_checkinteger(L, 2));
        lua_pushinteger(L, a + b);
        lua_pushinteger(L, a * b);
        return 2;
    });
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto r = AWAIT(rt->CallFunction(fn_ref, {static_cast<int64_t>(3), static_cast<int64_t>(4)}));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 7);
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 12);
    rt->PushRelease({fn_ref});
}

TEST_F(LuaRuntimeTest, CallFunctionReturnsString) {
    lua_State* main_L = rt->lua().lua_state();
    lua_pushcfunction(main_L, [](lua_State* L) -> int {
        lua_pushstring(L, "hello from CallFunction");
        return 1;
    });
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto r = AWAIT(rt->CallFunction(fn_ref));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "hello from CallFunction");
    rt->PushRelease({fn_ref});
}

TEST_F(LuaRuntimeTest, CallFunctionReturnsTable) {
    lua_State* main_L = rt->lua().lua_state();
    AWAIT(rt->RunScript(R"(
        _test_add = function(a, b) return {sum = a + b, product = a * b} end
    )"));
    lua_getglobal(main_L, "_test_add");
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto r = AWAIT(rt->CallFunction(fn_ref, {static_cast<int64_t>(5), static_cast<int64_t>(6)}));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<LuaRef>(r.values[0]));
    // Can't easily verify table contents from C++, just check type
    EXPECT_EQ(std::get<LuaRef>(r.values[0])->type, LUA_TTABLE);
    rt->PushRelease({fn_ref});
}

TEST_F(LuaRuntimeTest, CallFunctionReturnsError) {
    lua_State* main_L = rt->lua().lua_state();
    lua_pushcfunction(main_L, [](lua_State* L) -> int {
        return luaL_error(L, "intentional error");
    });
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto r = AWAIT(rt->CallFunction(fn_ref));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_NE(r.error.find("intentional error"), std::string::npos);
    rt->PushRelease({fn_ref});
}

TEST_F(LuaRuntimeTest, CallFunctionNoArgs) {
    lua_State* main_L = rt->lua().lua_state();
    lua_pushcfunction(main_L, [](lua_State* L) -> int {
        lua_pushinteger(L, 42);
        return 1;
    });
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto r = AWAIT(rt->CallFunction(fn_ref));
    EXPECT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 42);
    rt->PushRelease({fn_ref});
}

TEST_F(LuaRuntimeTest, CallFunctionCanYield) {
    lua_State* main_L = rt->lua().lua_state();
    lua_pushcfunction(main_L, [](lua_State* L) -> int {
        auto ctx = LuaRuntime::FromLuaState(L);
        int ms = static_cast<int>(luaL_checkinteger(L, 1));
        auto handle = ctx->PreYield(L);
        std::thread([ctx, handle, ms]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            ctx->PushResume(handle);
        }).detach();
        return ctx->Yield(L);
    });
    int fn_ref = luaL_ref(main_L, LUA_REGISTRYINDEX);

    auto start = std::chrono::steady_clock::now();
    auto r = AWAIT(rt->CallFunction(fn_ref, {static_cast<int64_t>(50)}));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(r.status, LUA_OK);
    EXPECT_GE(elapsed, 30);
    rt->PushRelease({fn_ref});
}

// --- Factory: shared config across multiple runtimes ---

class LuaRuntimeBuilderTest : public ::testing::Test {
protected:
    static int luaopen_testmath(lua_State* L) {
        lua_newtable(L);
        lua_pushcfunction(L, [](lua_State* L) -> int {
            int a = static_cast<int>(luaL_checkinteger(L, 1));
            int b = static_cast<int>(luaL_checkinteger(L, 2));
            lua_pushinteger(L, a * b);
            return 1;
        });
        lua_setfield(L, -2, "mul");
        return 1;
    }

    void SetUp() override {
        auto p = std::make_unique<TestCodeProvider>();
        provider = p.get();
        provider->set_module("greet", "return { hello = function() return 'hi' end }");

        factory = std::make_unique<LuaRuntime::Builder>();
        factory->WithCodeProvider(std::move(p));
        factory->Register("testmath", luaopen_testmath);
    }

    TestCodeProvider* provider = nullptr;
    std::unique_ptr<LuaRuntime::Builder> factory;
};

TEST_F(LuaRuntimeBuilderTest, MultipleRuntimesShareCodeProvider) {
    auto rt1 = factory->Create();
    auto rt2 = factory->Create();

    EXPECT_EQ(AWAIT(rt1->RunScript("local g = require('greet'); assert(g.hello() == 'hi')")).status, LUA_OK);
    EXPECT_EQ(AWAIT(rt2->RunScript("local g = require('greet'); assert(g.hello() == 'hi')")).status, LUA_OK);
}

TEST_F(LuaRuntimeBuilderTest, MultipleRuntimesShareCModules) {
    auto rt1 = factory->Create();
    auto rt2 = factory->Create();

    EXPECT_EQ(AWAIT(rt1->RunScript("local m = require('testmath'); assert(m.mul(3, 4) == 12)")).status, LUA_OK);
    EXPECT_EQ(AWAIT(rt2->RunScript("local m = require('testmath'); assert(m.mul(5, 6) == 30)")).status, LUA_OK);
}

TEST_F(LuaRuntimeBuilderTest, MultipleRuntimesAreIndependent) {
    auto rt1 = factory->Create();
    auto rt2 = factory->Create();

    rt1->lua()["x"] = 100;
    rt2->lua()["x"] = 200;

    EXPECT_EQ(AWAIT(rt1->RunScript("assert(x == 100)")).status, LUA_OK);
    EXPECT_EQ(AWAIT(rt2->RunScript("assert(x == 200)")).status, LUA_OK);
}

// --- LuaExtension ---

class TestExtension : public LuaExtension {
public:
    void OnInit(lua_State* L) override {
        lua_pushinteger(L, 42);
        lua_setglobal(L, "magic_number");
        init_count++;
    }
    void OnShutdown(lua_State* L) override { shutdown_count++; }

    int init_count = 0;
    int shutdown_count = 0;
};

TEST_F(LuaRuntimeBuilderTest, ExtensionOnInitCalled) {
    auto ext = std::make_shared<TestExtension>();
    factory->RegisterExtension(ext);

    auto rt = factory->Create();
    EXPECT_EQ(ext->init_count, 1);
    EXPECT_EQ(AWAIT(rt->RunScript("assert(magic_number == 42)")).status, LUA_OK);
}

TEST_F(LuaRuntimeBuilderTest, ExtensionOnShutdownCalled) {
    auto ext = std::make_shared<TestExtension>();
    factory->RegisterExtension(ext);

    {
        auto rt = factory->Create();
        EXPECT_EQ(ext->shutdown_count, 0);
    }
    EXPECT_EQ(ext->shutdown_count, 1);
}

TEST_F(LuaRuntimeBuilderTest, ExtensionSharedAcrossRuntimes) {
    auto ext = std::make_shared<TestExtension>();
    factory->RegisterExtension(ext);

    auto rt1 = factory->Create();
    EXPECT_EQ(ext->init_count, 1);
    auto rt2 = factory->Create();
    EXPECT_EQ(ext->init_count, 2);

    EXPECT_EQ(AWAIT(rt1->RunScript("assert(magic_number == 42)")).status, LUA_OK);
    EXPECT_EQ(AWAIT(rt2->RunScript("assert(magic_number == 42)")).status, LUA_OK);
}

// --- LuaLibrary ---

class TestLibrary : public LuaLibrary {
public:
    std::string name() const override { return "testlib"; }

    void Open(lua_State* L) override {
        lua_newtable(L);
        lua_pushcfunction(L, [](lua_State* L) -> int {
            int a = static_cast<int>(luaL_checkinteger(L, 1));
            int b = static_cast<int>(luaL_checkinteger(L, 2));
            lua_pushinteger(L, a + b);
            return 1;
        });
        lua_setfield(L, -2, "add");
        lua_pushinteger(L, 99);
        lua_setfield(L, -2, "magic");
        open_count++;
    }

    void Close(lua_State* L) override { close_count++; }

    int open_count = 0;
    int close_count = 0;
};

class LuaLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        lib = std::make_shared<TestLibrary>();
        rt = LuaRuntime::Builder()
            .RegisterLibrary(lib)
            .Create();
    }

    std::shared_ptr<TestLibrary> lib;
    LuaRuntime::Ptr rt;
};

TEST_F(LuaLibraryTest, RequireReturnsTable) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local m = require("testlib")
        assert(type(m) == "table")
        assert(m.add(1, 2) == 3)
        assert(m.magic == 99)
    )")).status, LUA_OK);
}

TEST_F(LuaLibraryTest, RequireCachesResult) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = require("testlib")
        local b = require("testlib")
        assert(a == b, "expected same table on second require")
    )")).status, LUA_OK);
}

TEST_F(LuaLibraryTest, OpenCalledOnEachRequire) {
    // Open is called each time require is invoked (not cached by the library itself)
    EXPECT_EQ(lib->open_count, 0);
    AWAIT(rt->RunScript(R"(
        local a = require("testlib")
        local b = require("testlib")  -- cached, Open not called again
    )"));
    EXPECT_EQ(lib->open_count, 1);
}

TEST_F(LuaLibraryTest, CloseCalledOnShutdown) {
    EXPECT_EQ(lib->close_count, 0);
    rt.reset();
    EXPECT_EQ(lib->close_count, 1);
}

TEST_F(LuaLibraryTest, LibraryNotFound) {
    auto r = AWAIT(rt->RunScript(R"(require("nonexistent_lib"))"));
    EXPECT_NE(r.status, LUA_OK);
    EXPECT_FALSE(r.error.empty());
}

TEST_F(LuaLibraryTest, LibraryWithCodeProviderPrecedence) {
    auto provider = std::make_unique<TestCodeProvider>();
    provider->set_module("testlib", "return { from_provider = true }");

    auto rt2 = LuaRuntime::Builder()
        .RegisterLibrary(lib)
        .WithCodeProvider(std::move(provider))
        .Create();

    // Library takes precedence over CodeProvider
    EXPECT_EQ(AWAIT(rt2->RunScript(R"(
        local m = require("testlib")
        assert(m.magic == 99, "expected library, got provider module")
    )")).status, LUA_OK);
}

TEST_F(LuaLibraryTest, CModuleTakesPrecedenceOverLibrary) {
    rt = LuaRuntime::Builder()
        .Register("testlib", [](lua_State* L) -> int {
            lua_newtable(L);
            lua_pushstring(L, "from_cmodule");
            lua_setfield(L, -2, "source");
            return 1;
        })
        .RegisterLibrary(lib)
        .Create();

    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local m = require("testlib")
        assert(m.source == "from_cmodule", "expected C module to take precedence")
    )")).status, LUA_OK);
}

TEST_F(LuaLibraryTest, MultipleLibraries) {
    struct MathLib : public LuaLibrary {
        std::string name() const override { return "mymath"; }
        void Open(lua_State* L) override {
            lua_newtable(L);
            lua_pushcfunction(L, [](lua_State* L) -> int {
                int a = static_cast<int>(luaL_checkinteger(L, 1));
                int b = static_cast<int>(luaL_checkinteger(L, 2));
                lua_pushinteger(L, a * b);
                return 1;
            });
            lua_setfield(L, -2, "mul");
        }
    };

    auto rt2 = LuaRuntime::Builder()
        .RegisterLibrary(std::make_shared<MathLib>())
        .RegisterLibrary(lib)
        .Create();

    EXPECT_EQ(AWAIT(rt2->RunScript(R"(
        local m = require("mymath")
        local t = require("testlib")
        assert(m.mul(3, 4) == 12)
        assert(t.add(1, 2) == 3)
    )")).status, LUA_OK);
}
