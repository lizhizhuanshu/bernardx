#include "http_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

class HttpLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        work = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(ioc));
        exec = std::make_unique<coro_io::ExecutorWrapper<>>(ioc.get_executor());
        io_thread = std::thread([this]() { ioc.run(); });
        rt = LuaRuntime::Builder()
            .RegisterLibrary(std::make_shared<HttpLibrary>(*exec))
            .Create();
    }

    void TearDown() override {
        rt.reset();
        work.reset();
        ioc.stop();
        if (io_thread.joinable()) io_thread.join();
    }

    asio::io_context ioc{1};
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::unique_ptr<coro_io::ExecutorWrapper<>> exec;
    std::thread io_thread;
    LuaRuntime::Ptr rt;
};

TEST_F(HttpLibraryTest, RequireReturnsTable) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        assert(type(http) == "table")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, HasHttpFunctions) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        assert(type(http.get) == "function", "expected http.get")
        assert(type(http.post) == "function", "expected http.post")
        assert(type(http.put) == "function", "expected http.put")
        assert(type(http.del) == "function", "expected http.del")
        assert(type(http.ws_create) == "function", "expected http.ws_create")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, WsCreateReturnsUserdata) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        local ws = http.ws_create("wss://echo.websocket.org")
        assert(type(ws) == "userdata", "expected userdata for ws")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, WsSupportsCallbackAssignment) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        local ws = http.ws_create("wss://echo.websocket.org")
        local received = nil
        ws.onmessage = function(data) received = data end
        ws.onerror = function(err) end
        ws.onclose = function() end
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, WsCallbackReadback) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        local ws = http.ws_create("wss://echo.websocket.org")
        local fn = function(data) end
        ws.onmessage = fn
        assert(ws.onmessage == fn, "expected same function back")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, WsInvalidPropertyErrors) {
    auto r = AWAIT(rt->RunScript(R"(
        local http = require("http")
        local ws = http.ws_create("wss://echo.websocket.org")
        ws.invalid_prop = 42
    )"));
    EXPECT_NE(r.status, LUA_OK);
}

TEST_F(HttpLibraryTest, HttpGetReturnsErrorForInvalidUrl) {
    auto r = AWAIT(rt->RunScript(R"(
        local http = require("http")
        local status, body, err = http.get("http://127.0.0.1:1")
        assert(err ~= nil, "expected error message for connection refused, got nil")
    )"));
    EXPECT_EQ(r.status, LUA_OK);
}

TEST_F(HttpLibraryTest, RequireCached) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = require("http")
        local b = require("http")
        assert(a == b, "expected same table on second require")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, WsConnectToInvalidEndpoint) {
    auto r = AWAIT(rt->RunScript(R"(
        local http = require("http")
        local ws = http.ws_create("ws://127.0.0.1:1")
        local ok, err = ws:connect()
        assert(ok == false, "expected connect to fail")
    )"));
    EXPECT_EQ(r.status, LUA_OK);
}

#ifdef __linux__
TEST(HttpLibraryStressTest, RecreateRuntimeWithHttpLibrary100TimesWithTimeoutGuard) {
    constexpr int kIterations = 100;
    constexpr auto kTimeout = std::chrono::seconds(30);
    constexpr auto kPollInterval = std::chrono::milliseconds(10);

    pid_t pid = fork();
    ASSERT_NE(pid, -1) << "fork failed";

    if (pid == 0) {
        asio::io_context ioc{1};
        auto work = asio::make_work_guard(ioc);
        auto exec = std::make_unique<coro_io::ExecutorWrapper<>>(ioc.get_executor());
        std::thread io_thread([&ioc]() { ioc.run(); });

        for (int i = 0; i < kIterations; ++i) {
            auto runtime = LuaRuntime::Builder()
                .RegisterLibrary(std::make_shared<HttpLibrary>(*exec))
                .Create();
            auto result = AWAIT(runtime->RunScript(R"(
                local http = require("http")
                local ws = http.ws_create("ws://127.0.0.1:1")
                ws.onclose = function() end
                ws.onerror = function() end
                local ok = select(1, ws:connect())
                assert(ok == false, "expected invalid endpoint connect to fail")
            )"));
            if (result.status != LUA_OK) {
                _exit(2);
            }
        }

        work.reset();
        ioc.stop();
        io_thread.join();
        _exit(0);
    }

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) {
            if (WIFEXITED(status)) {
                EXPECT_EQ(WEXITSTATUS(status), 0)
                    << "child exited with non-zero status, possible HttpLibrary teardown failure";
            } else {
                FAIL() << "child did not exit normally";
            }
            return;
        }
        ASSERT_NE(wait_result, -1) << "waitpid failed";
        std::this_thread::sleep_for(kPollInterval);
    }

    const pid_t final_wait_result = waitpid(pid, &status, WNOHANG);
    if (final_wait_result == pid) {
        if (WIFEXITED(status)) {
            EXPECT_EQ(WEXITSTATUS(status), 0)
                << "child exited with non-zero status near timeout boundary";
        } else {
            FAIL() << "child did not exit normally near timeout boundary";
        }
        return;
    }
    ASSERT_NE(final_wait_result, -1) << "waitpid failed on final check";

    const int kill_result = kill(pid, SIGKILL);
    if (kill_result == -1) {
        if (errno == ESRCH) {
            ASSERT_NE(waitpid(pid, &status, 0), -1) << "waitpid failed while reaping exited child";
            FAIL() << "child exited near timeout boundary";
        } else {
            FAIL() << "kill failed with errno " << errno;
        }
    } else {
        ASSERT_NE(waitpid(pid, &status, 0), -1) << "waitpid failed after SIGKILL";
        EXPECT_TRUE(WIFSIGNALED(status));
        EXPECT_EQ(WTERMSIG(status), SIGKILL);
    }

    FAIL() << "timeout while recreating LuaRuntime with HttpLibrary " << kIterations
           << " times (possible teardown hang)";
}
#endif
