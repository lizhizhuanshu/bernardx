#include "http_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

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

// Mini HTTP forward proxy for proxy-routing tests: accepts one connection on
// the fixture io_context, reads the full request head, lets a callback decide
// the canned response, and keeps the captured head for post-hoc assertions.
class MiniProxy {
public:
    explicit MiniProxy(asio::io_context& ioc)
        : acceptor_(ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)) {}

    uint16_t port() const { return acceptor_.local_endpoint().port(); }

    void start(std::function<std::string(const std::string&)> respond) {
        auto socket = std::make_shared<asio::ip::tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket,
            [this, socket, respond = std::move(respond)](std::error_code ec) mutable {
                if (ec) return;
                auto buf = std::make_shared<asio::streambuf>();
                asio::async_read_until(*socket, *buf, "\r\n\r\n",
                    [this, socket, buf, respond = std::move(respond)](std::error_code ec, size_t) mutable {
                        if (ec) return;
                        request_head_.assign(asio::buffers_begin(buf->data()), asio::buffers_end(buf->data()));
                        std::string response = respond(request_head_);
                        asio::async_write(*socket, asio::buffer(response),
                            [socket](std::error_code, size_t) {});
                    });
            });
    }

    const std::string& request_head() const { return request_head_; }

private:
    asio::ip::tcp::acceptor acceptor_;
    std::string request_head_;
};

TEST_F(HttpLibraryTest, RequireReturnsTable) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        assert(type(http) == "table")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, HasRequestFunction) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        assert(type(http.request) == "function", "expected http.request")
        assert(type(http.ws_create) == "function", "expected http.ws_create")
        -- verb shortcuts were removed in favor of http.request
        assert(http.get == nil, "http.get should be removed")
        assert(http.post == nil, "http.post should be removed")
        assert(http.put == nil, "http.put should be removed")
        assert(http.del == nil, "http.del should be removed")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, RequestRequiresUrl) {
    auto r = AWAIT(rt->RunScript(R"(
        local http = require("http")
        http.request({})
    )"));
    EXPECT_NE(r.status, LUA_OK);
}

TEST_F(HttpLibraryTest, RequestDispatchesAllMethods) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")
        for _, m in ipairs({"GET", "get", "POST", "PUT", "DELETE"}) do
            local status, body, err = http.request({url = "http://127.0.0.1:1", method = m})
            assert(err ~= nil, "expected error for method " .. m)
        end
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

TEST_F(HttpLibraryTest, HttpRequestReturnsErrorForInvalidUrl) {
    auto r = AWAIT(rt->RunScript(R"(
        local http = require("http")
        local status, body, err = http.request({url = "http://127.0.0.1:1"})
        assert(err ~= nil, "expected error message for connection refused, got nil")
    )"));
    EXPECT_EQ(r.status, LUA_OK);
}

TEST_F(HttpLibraryTest, RequestHonorsTimeoutMs) {
    // Blackhole server: accepts the connection but never responds, so the only
    // way the request completes is via the req-timeout we set below.
    asio::ip::tcp::acceptor acceptor(
        ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto port = acceptor.local_endpoint().port();
    asio::ip::tcp::socket sink(ioc);
    acceptor.async_accept(sink, [](const std::error_code&) {});

    std::string script = R"(
        local http = require("http")
        local status, body, err = http.request({
            url = "http://127.0.0.1:__PORT__/",
            timeout = 100,
        })
        assert(err ~= nil, "expected timeout error")
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(port));

    const auto start = std::chrono::steady_clock::now();
    auto r = AWAIT(rt->RunScript(script));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();

    EXPECT_EQ(r.status, LUA_OK);
    // Resolved by our 100ms timeout, not cinatra's 60s default.
    EXPECT_LT(elapsed_ms, 5000);
}

TEST_F(HttpLibraryTest, RequestGoesThroughProxy) {
    MiniProxy proxy(ioc);
    proxy.start([](const std::string&) {
        return std::string("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nvia-proxy");
    });

    // The target host is unresolvable (.invalid TLD), so the only way this
    // request can succeed is by reaching the local proxy.
    std::string script = R"(
        local http = require("http")
        local status, body, err = http.request({
            url = "http://nonexistent.invalid/api",
            proxy = "http://127.0.0.1:__PORT__",
        })
        assert(err == nil, "unexpected error: " .. tostring(err))
        assert(status == 200, "expected 200, got " .. tostring(status))
        assert(body == "via-proxy", "unexpected body: " .. tostring(body))
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(proxy.port()));

    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);

    // Absolute-form request line proves the client sent the request to the proxy.
    EXPECT_NE(proxy.request_head().find("GET http://nonexistent.invalid"), std::string::npos)
        << "proxy saw request head: " << proxy.request_head();
}

TEST_F(HttpLibraryTest, ProxyBasicAuthSendsAuthorization) {
    MiniProxy proxy(ioc);
    proxy.start([](const std::string& head) {
        // base64("user:pass") == "dXNlcjpwYXNz"
        if (head.find("Proxy-Authorization: Basic dXNlcjpwYXNz") != std::string::npos) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nauth-ok");
        }
        return std::string("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n");
    });

    std::string script = R"(
        local http = require("http")
        local status, body, err = http.request({
            url = "http://nonexistent.invalid/api",
            proxy = "http://user:pass@127.0.0.1:__PORT__",
        })
        assert(err == nil, "unexpected error: " .. tostring(err))
        assert(status == 200, "proxy did not accept our credentials, got " .. tostring(status))
        assert(body == "auth-ok", "unexpected body: " .. tostring(body))
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(proxy.port()));

    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

TEST_F(HttpLibraryTest, ProxyInvalidInputRaisesLuaError) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local http = require("http")

        -- non-http schemes are rejected (cinatra speaks HTTP forward proxy only)
        local ok, err = pcall(http.request, {
            url = "http://127.0.0.1:1/",
            proxy = "socks5://127.0.0.1:1080",
        })
        assert(ok == false, "expected pcall to fail for socks5 proxy")
        assert(tostring(err):find("socks5", 1, true) ~= nil,
               "error should mention the scheme: " .. tostring(err))

        -- empty host is also a parse error
        ok, err = pcall(http.request, {url = "http://127.0.0.1:1/", proxy = "http://"})
        assert(ok == false, "expected pcall to fail for empty proxy host")

        -- same validation applies to ws_create options
        ok, err = pcall(http.ws_create, "ws://127.0.0.1:1/", {proxy = "socks5://127.0.0.1:1080"})
        assert(ok == false, "expected pcall to fail for ws_create socks5 proxy")
    )")).status, LUA_OK);
}

TEST_F(HttpLibraryTest, WsConnectThroughProxy) {
    MiniProxy proxy(ioc);
    proxy.start([](const std::string&) {
        // Minimal 101 upgrade reply; the client does not validate the accept key.
        return std::string("HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                           "\r\n");
    });

    std::string script = R"(
        local http = require("http")
        local ws = http.ws_create("ws://nonexistent.invalid/echo", {
            proxy = "http://127.0.0.1:__PORT__",
        })
        ws.onclose = function() end
        ws.onerror = function() end
        local ok = select(1, ws:connect())
        assert(ok == true, "expected ws connect via proxy to succeed")
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(proxy.port()));

    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);

    // The upgrade request reached the proxy in absolute form with ws headers.
    EXPECT_NE(proxy.request_head().find("GET http://nonexistent.invalid"), std::string::npos)
        << "proxy saw request head: " << proxy.request_head();
    EXPECT_NE(proxy.request_head().find("Upgrade: websocket"), std::string::npos)
        << "proxy saw request head: " << proxy.request_head();
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
