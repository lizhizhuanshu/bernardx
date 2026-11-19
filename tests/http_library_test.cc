#include "http_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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

// HTTP echo server: per connection, reads one full request (head + exactly
// Content-Length body bytes) and echoes the raw body bytes back, so Lua can
// assert byte-exact round-trips. Catches silent truncation at the first
// embedded NUL — the wire length would shrink and the echo would mismatch.
class EchoServer {
public:
    explicit EchoServer(asio::io_context& ioc)
        : acceptor_(ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)) {}

    uint16_t port() const { return acceptor_.local_endpoint().port(); }

    void start() { accept_one(); }

private:
    // Multi-shot: each http.request uses a fresh client/connection, so the
    // acceptor re-arms after every hit for the duration of the test.
    void accept_one() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](std::error_code ec) mutable {
            if (ec) return;
            accept_one();
            read_head(socket);
        });
    }

    static void read_head(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        auto buf = std::make_shared<asio::streambuf>();
        asio::async_read_until(*socket, *buf, "\r\n\r\n",
            [socket, buf](std::error_code ec, size_t) mutable {
                if (ec) return;
                // read_until may overshoot into the body; consume only the
                // head, leave the buffered body prefix in the streambuf.
                std::string all(asio::buffers_begin(buf->data()), asio::buffers_end(buf->data()));
                size_t head_len = all.find("\r\n\r\n") + 4;
                buf->consume(head_len);

                // Content-Length is case-insensitive in principle; lower a copy
                // and reuse the index into the original head.
                std::string head = all.substr(0, head_len);
                std::string lower;
                lower.reserve(head.size());
                for (char c : head) {
                    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }
                size_t content_len = 0;
                if (auto pos = lower.find("content-length:"); pos != std::string::npos) {
                    content_len = std::strtoull(head.c_str() + pos + strlen("content-length:"),
                                                nullptr, 10);
                }

                // transfer_exactly counts only bytes moved by THIS read; ask
                // for what is still missing on top of the buffered prefix.
                size_t buffered = buf->size();
                asio::async_read(*socket, *buf,
                    asio::transfer_exactly(content_len > buffered ? content_len - buffered : 0),
                    [socket, buf, content_len](std::error_code ec, size_t) mutable {
                        if (ec) return;
                        auto begin = asio::buffers_begin(buf->data());
                        std::string body(begin, begin + static_cast<std::ptrdiff_t>(content_len));
                        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                               std::to_string(body.size()) + "\r\n\r\n" + body;
                        asio::async_write(*socket, asio::buffer(response),
                            [socket](std::error_code, size_t) {});
                    });
            });
    }

    asio::ip::tcp::acceptor acceptor_;
};

// Configurable multi-shot HTTP server for http.download tests: accepts any
// number of connections (download uses a fresh client per redirect hop);
// per request a callback sees the raw request head and returns the canned
// response bytes.
class FileServer {
public:
    explicit FileServer(asio::io_context& ioc)
        : acceptor_(ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)) {}

    uint16_t port() const { return acceptor_.local_endpoint().port(); }

    void start(std::function<std::string(const std::string&)> respond) {
        respond_ = std::move(respond);
        accept_one();
    }

    int hits() const { return hits_; }

private:
    void accept_one() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](std::error_code ec) mutable {
            if (ec) return;
            accept_one();
            read_head(socket);
        });
    }

    // One body-less request per connection is all the download flow sends.
    void read_head(const std::shared_ptr<asio::ip::tcp::socket>& socket) {
        auto buf = std::make_shared<asio::streambuf>();
        asio::async_read_until(*socket, *buf, "\r\n\r\n",
            [this, socket, buf](std::error_code ec, size_t) mutable {
                if (ec) return;
                std::string head(asio::buffers_begin(buf->data()), asio::buffers_end(buf->data()));
                ++hits_;
                std::string response = respond_(head);
                asio::async_write(*socket, asio::buffer(response),
                    [socket](std::error_code, size_t) {});
            });
    }

    asio::ip::tcp::acceptor acceptor_;
    std::function<std::string(const std::string&)> respond_;
    int hits_ = 0;
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
        assert(type(http.download) == "function", "expected http.download")
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

TEST_F(HttpLibraryTest, RequestBodySurvivesEmbeddedNuls) {
    EchoServer server(ioc);
    server.start();

    // Leading NUL (pre-fix truncation cut there), embedded NULs, high bytes,
    // and a >4KB tail so the payload crosses cinatra's buffer boundaries.
    std::string script = R"(
        local http = require("http")
        local payload = "\0ab\0cd\0\0z" .. string.rep("\1\2\255\0", 1024)
        assert(#payload == 9 + 4 * 1024, "payload length")
        for _, m in ipairs({"POST", "PUT"}) do
            local status, body, err = http.request({
                url = "http://127.0.0.1:__PORT__/upload",
                method = m,
                body = payload,
                content_type = "octet",
            })
            assert(err == nil, m .. " unexpected error: " .. tostring(err))
            assert(status == 200, m .. " expected 200, got " .. tostring(status))
            assert(body == payload,
                   m .. " body round-trip mismatch: got " .. #body .. " bytes, want " .. #payload)
        end
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(server.port()));

    auto r = AWAIT(rt->RunScript(script));
    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);
}

// --- http.download ---

static std::string CannedResponse(
    int status, const std::string& reason,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body) {
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    for (const auto& [k, v] : headers) r += k + ": " + v + "\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return r;
}

static std::string DownloadTempDir(const char* tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("http_dl_" + std::to_string(::getpid()) + "_" + tag);
    std::filesystem::remove_all(dir);
    return dir.string();
}

static bool ReadFileBytes(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Replaces every __PORT__/__PATH__ occurrence (paths appear in both the
// options table and the assertions).
static std::string FillScript(std::string script, uint16_t port, const std::string& path) {
    for (auto pos = script.find("__PORT__"); pos != std::string::npos;
         pos = script.find("__PORT__"))
        script.replace(pos, 8, std::to_string(port));
    for (auto pos = script.find("__PATH__"); pos != std::string::npos;
         pos = script.find("__PATH__"))
        script.replace(pos, 8, path);
    return script;
}

TEST_F(HttpLibraryTest, DownloadSavesBodyToFile) {
    FileServer server(ioc);
    // Embedded NUL/high bytes + a >4KB tail crossing cinatra's buffers.
    const std::string body = std::string("BIN\0\1\255", 7) + std::string(8192, 'x');
    server.start([&](const std::string&) { return CannedResponse(200, "OK", {}, body); });

    // Nested fresh directories exercise parent-dir creation.
    std::string dir = DownloadTempDir("basic");
    std::string path = dir + "/sub/dir/file.bin";
    auto r = AWAIT(rt->RunScript(FillScript(R"(
        local http = require("http")
        local status, p, err = http.download({
            url = "http://127.0.0.1:__PORT__/f.bin",
            path = "__PATH__",
        })
        assert(status == 200, "status " .. tostring(status))
        assert(p == "__PATH__", "returned path " .. tostring(p))
        assert(err == nil, "err " .. tostring(err))
    )", server.port(), path)));
    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);

    std::string got;
    ASSERT_TRUE(ReadFileBytes(path, &got));
    EXPECT_EQ(got, body);
    EXPECT_EQ(server.hits(), 1);
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove_all(dir);
}

TEST_F(HttpLibraryTest, DownloadFollowsRelativeRedirect) {
    FileServer server(ioc);
    const std::string body = "redirected payload";
    server.start([&](const std::string& head) {
        if (head.rfind("GET /latest") == 0) {
            return CannedResponse(302, "Found", {{"Location", "/f-1.2.3.bin"}}, "");
        }
        return CannedResponse(200, "OK", {}, body);
    });

    std::string dir = DownloadTempDir("redir");
    std::string path = dir + "/file.bin";
    auto r = AWAIT(rt->RunScript(FillScript(R"(
        local http = require("http")
        local status, p, err = http.download({
            url = "http://127.0.0.1:__PORT__/latest",
            path = "__PATH__",
        })
        assert(status == 200, "status " .. tostring(status))
        assert(err == nil, "err " .. tostring(err))
    )", server.port(), path)));
    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);

    std::string got;
    ASSERT_TRUE(ReadFileBytes(path, &got));
    EXPECT_EQ(got, body);
    EXPECT_EQ(server.hits(), 2);
    std::filesystem::remove_all(dir);
}

TEST_F(HttpLibraryTest, DownloadRedirectDropsHeadersCrossHost) {
    // A second port is a different authority: custom headers must follow the
    // first hop but never the cross-host redirect.
    FileServer origin(ioc), cdn(ioc);
    const std::string body = "cdn payload";
    bool origin_saw_token = false, cdn_saw_token = false;
    origin.start([&](const std::string& head) {
        origin_saw_token = head.find("X-Token") != std::string::npos;
        return CannedResponse(302, "Found",
                              {{"Location", "http://127.0.0.1:" + std::to_string(cdn.port()) + "/f.bin"}},
                              "");
    });
    cdn.start([&](const std::string& head) {
        cdn_saw_token = head.find("X-Token") != std::string::npos;
        return CannedResponse(200, "OK", {}, body);
    });

    std::string dir = DownloadTempDir("crosshost");
    std::string path = dir + "/file.bin";
    auto r = AWAIT(rt->RunScript(FillScript(R"(
        local http = require("http")
        local status, p, err = http.download({
            url = "http://127.0.0.1:__PORT__/latest",
            path = "__PATH__",
            headers = {["X-Token"] = "secret"},
        })
        assert(status == 200, "status " .. tostring(status))
        assert(err == nil, "err " .. tostring(err))
    )", origin.port(), path)));
    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);

    EXPECT_TRUE(origin_saw_token);
    EXPECT_FALSE(cdn_saw_token);
    std::string got;
    ASSERT_TRUE(ReadFileBytes(path, &got));
    EXPECT_EQ(got, body);
    std::filesystem::remove_all(dir);
}

TEST_F(HttpLibraryTest, DownloadHttpErrorLeavesNoFile) {
    FileServer server(ioc);
    server.start(
        [&](const std::string&) { return CannedResponse(404, "Not Found", {}, "nope"); });

    std::string dir = DownloadTempDir("http404");
    std::string path = dir + "/file.bin";
    auto r = AWAIT(rt->RunScript(FillScript(R"(
        local http = require("http")
        local status, p, err = http.download({
            url = "http://127.0.0.1:__PORT__/missing.bin",
            path = "__PATH__",
        })
        assert(status == 404, "status " .. tostring(status))
        assert(p == nil, "expected nil path, got " .. tostring(p))
        assert(err ~= nil and err:find("404"), "err " .. tostring(err))
    )", server.port(), path)));
    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);

    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove_all(dir);
}

TEST_F(HttpLibraryTest, DownloadNetErrorKeepsExistingFile) {
    std::string dir = DownloadTempDir("neterr");
    std::filesystem::create_directories(dir);
    std::string path = dir + "/file.bin";
    { std::ofstream(path, std::ios::binary) << "OLD CONTENT"; }

    // Port 1 has no listener (same dead-endpoint convention as the request
    // tests): the download must fail without touching the existing file.
    auto r = AWAIT(rt->RunScript(FillScript(R"(
        local http = require("http")
        local status, p, err = http.download({
            url = "http://127.0.0.1:1/file.bin",
            path = "__PATH__",
        })
        assert(status == 0, "status " .. tostring(status))
        assert(p == nil, "expected nil path, got " .. tostring(p))
        assert(err ~= nil, "expected error")
    )", 1, path)));
    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);

    std::string got;
    ASSERT_TRUE(ReadFileBytes(path, &got));
    EXPECT_EQ(got, "OLD CONTENT");
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove_all(dir);
}

TEST_F(HttpLibraryTest, DownloadTooManyRedirects) {
    FileServer server(ioc);
    server.start([&](const std::string&) {
        return CannedResponse(302, "Found", {{"Location", "/loop"}}, "");
    });

    std::string dir = DownloadTempDir("loop");
    std::string path = dir + "/file.bin";
    auto r = AWAIT(rt->RunScript(FillScript(R"(
        local http = require("http")
        local status, p, err = http.download({
            url = "http://127.0.0.1:__PORT__/loop",
            path = "__PATH__",
        })
        assert(status == 302, "status " .. tostring(status))
        assert(p == nil, "expected nil path")
        assert(err == "too many redirects", "err " .. tostring(err))
    )", server.port(), path)));
    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);

    EXPECT_EQ(server.hits(), 5);  // 5 hops max, no file written
    EXPECT_FALSE(std::filesystem::exists(path));
    std::filesystem::remove_all(dir);
}

// Opt-in real-network smoke (HTTP_DOWNLOAD_E2E=1): the suite stays hermetic
// by default (CI sandboxes have no outbound network); this one deliberately
// hits boring high-uptime endpoints — example.com over plain http and TLS,
// then github.com's permanent http→https redirect.
TEST_F(HttpLibraryTest, DownloadRealNetworkE2E) {
    if (!std::getenv("HTTP_DOWNLOAD_E2E")) {
        GTEST_SKIP() << "set HTTP_DOWNLOAD_E2E=1 to run the real-network download smoke";
    }
    std::string dir = DownloadTempDir("e2e");

    // 1. plain http: IANA's example.com — tiny and effectively never down.
    {
        std::string path = dir + "/example_http.html";
        auto r = AWAIT(rt->RunScript(FillScript(R"(
            local http = require("http")
            local status, p, err = http.download({
                url = "http://example.com/",
                path = "__PATH__",
                timeout = 30000,
            })
            assert(err == nil, "err " .. tostring(err))
            assert(status == 200, "status " .. tostring(status))
        )", 1, path)));
        ASSERT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);
        std::string got;
        ASSERT_TRUE(ReadFileBytes(path, &got));
        EXPECT_NE(got.find("Example Domain"), std::string::npos) << got.substr(0, 128);
    }

    // 2. https: same resource over TLS. Without OpenSSL in the build every
    //    https URL fails with "Protocol error" (documented backend matrix) —
    //    surfaced as NOSSL below and skipped rather than failed.
    {
        std::string path = dir + "/example_https.html";
        auto r = AWAIT(rt->RunScript(FillScript(R"(
            local http = require("http")
            local status, p, err = http.download({
                url = "https://example.com/",
                path = "__PATH__",
                timeout = 30000,
            })
            if err and err:find("Protocol error") then
                error("NOSSL")  -- build without OpenSSL; skip, not fail
            end
            assert(err == nil, "err " .. tostring(err))
            assert(status == 200, "status " .. tostring(status))
        )", 1, path)));
        if (r.status != LUA_OK && r.error.find("NOSSL") != std::string::npos) {
            std::filesystem::remove_all(dir);
            GTEST_SKIP() << "build without OpenSSL: https unavailable";
        }
        ASSERT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);
        std::string got;
        ASSERT_TRUE(ReadFileBytes(path, &got));
        EXPECT_NE(got.find("Example Domain"), std::string::npos) << got.substr(0, 128);
    }

    // 3. a real redirect: github.com serves plain http as 301 → https.
    //    Content varies, so only assert it landed and is non-trivial.
    {
        std::string path = dir + "/github.html";
        auto r = AWAIT(rt->RunScript(FillScript(R"(
            local http = require("http")
            local status, p, err = http.download({
                url = "http://github.com/",
                path = "__PATH__",
                timeout = 60000,
            })
            assert(err == nil, "err " .. tostring(err))
            assert(status == 200, "status " .. tostring(status))
        )", 1, path)));
        ASSERT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);
        std::string got;
        ASSERT_TRUE(ReadFileBytes(path, &got));
        EXPECT_GT(got.size(), 1000u);
    }

    std::filesystem::remove_all(dir);
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

TEST_F(HttpLibraryTest, RequestAutoEncodesUrl) {
    MiniProxy proxy(ioc);
    proxy.start([](const std::string&) {
        return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    });

    // Space in path and query, plus UTF-8 in a value: all rejected by
    // cinatra's URI whitelist pre-normalization.
    std::string script = R"(
        local http = require("http")
        local status, body, err = http.request({
            url = "http://nonexistent.invalid/pro xy/?country=US&city=LAS VEGAS&user=\xe5\xbc\xa0\xe4\xb8\x89",
            proxy = "http://127.0.0.1:__PORT__",
        })
        assert(err == nil, "unexpected error: " .. tostring(err))
        assert(status == 200, "expected 200, got " .. tostring(status))
        assert(body == "ok", "unexpected body: " .. tostring(body))
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(proxy.port()));

    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);

    const std::string& head = proxy.request_head();
    // cinatra's proxy absolute-form pins the default port (:80).
    EXPECT_NE(head.find("GET http://nonexistent.invalid:80/pro%20xy/"), std::string::npos)
        << "path not encoded, head: " << head;
    EXPECT_NE(head.find("city=LAS%20VEGAS"), std::string::npos) << "head: " << head;
    EXPECT_NE(head.find("user=%E5%BC%A0%E4%B8%89"), std::string::npos) << "head: " << head;
}

TEST_F(HttpLibraryTest, RequestDoesNotDoubleEncodeUrl) {
    MiniProxy proxy(ioc);
    proxy.start([](const std::string&) {
        return std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    });

    std::string script = R"(
        local http = require("http")
        local status, body, err = http.request({
            url = "http://nonexistent.invalid/proxy/?city=LAS%20VEGAS",
            proxy = "http://127.0.0.1:__PORT__",
        })
        assert(err == nil, "unexpected error: " .. tostring(err))
        assert(status == 200, "expected 200, got " .. tostring(status))
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(proxy.port()));

    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);

    // Valid %XX escapes pass through verbatim — no %2520-style double encoding.
    const std::string& head = proxy.request_head();
    EXPECT_NE(head.find("city=LAS%20VEGAS"), std::string::npos) << "head: " << head;
    EXPECT_EQ(head.find("%25"), std::string::npos) << "double-encoded escape in head: " << head;
}

TEST_F(HttpLibraryTest, WsConnectAutoEncodesUrl) {
    MiniProxy proxy(ioc);
    proxy.start([](const std::string&) {
        return std::string("HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                           "\r\n");
    });

    std::string script = R"(
        local http = require("http")
        local ws = http.ws_create("ws://nonexistent.invalid/room name?user=\xe5\xbc\xa0\xe4\xb8\x89", {
            proxy = "http://127.0.0.1:__PORT__",
        })
        ws.onclose = function() end
        ws.onerror = function() end
        local ok = select(1, ws:connect())
        assert(ok == true, "expected ws connect to succeed")
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(proxy.port()));

    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);

    const std::string& head = proxy.request_head();
    EXPECT_NE(head.find("GET http://nonexistent.invalid:80/room%20name?user=%E5%BC%A0%E4%B8%89"),
              std::string::npos)
        << "ws url not encoded, head: " << head;
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

TEST_F(HttpLibraryTest, WsConnectFailureReturnsErrorMessage) {
    auto r = AWAIT(rt->RunScript(R"(
        local http = require("http")
        local ws = http.ws_create("ws://127.0.0.1:1")
        ws.onerror = function() end
        local ok, err = ws:connect()
        assert(ok == false, "expected connect to fail")
        assert(err ~= nil, "expected an error message, got nil")
    )"));
    EXPECT_EQ(r.status, LUA_OK);
}

TEST_F(HttpLibraryTest, WsConnectHonorsConnectTimeout) {
    // Blackhole server: accepts the TCP connection but never answers the
    // websocket upgrade, so only our connect_timeout can finish the wait.
    asio::ip::tcp::acceptor acceptor(
            ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto port = acceptor.local_endpoint().port();
    asio::ip::tcp::socket sink(ioc);
    acceptor.async_accept(sink, [](const std::error_code&) {});

    std::string script = R"(
        local http = require("http")
        local ws = http.ws_create("ws://127.0.0.1:__PORT__/", {connect_timeout = 200})
        ws.onerror = function() end
        local ok, err = ws:connect()
        assert(ok == false, "expected connect to time out")
        assert(tostring(err):find("timeout", 1, true) ~= nil,
               "expected timeout error, got " .. tostring(err))

        -- junk / negative timeout values fall back to the default without error
        local ws2 = http.ws_create("ws://127.0.0.1:1", {connect_timeout = "abc"})
        assert(select(1, ws2:connect()) == false)
        local ws3 = http.ws_create("ws://127.0.0.1:1", {connect_timeout = -1, idle_timeout = -5})
        assert(select(1, ws3:connect()) == false)
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(port));

    const auto start = std::chrono::steady_clock::now();
    auto r = AWAIT(rt->RunScript(script));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();

    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);
    // Resolved by our 200ms timer, not cinatra's 30s connect default.
    EXPECT_LT(elapsed_ms, 5000);
}

TEST_F(HttpLibraryTest, WsIdleTimeoutDropsQuietConnection) {
    // Answers the websocket upgrade, then holds the connection open without
    // ever sending: the only thing that can drop it is idle_timeout.
    // (A pending async_read_some keeps the accepted socket alive — without a
    // pending handler the socket's last shared_ptr dies and the client sees
    // an instant EOF instead of a timeout.)
    static const std::string kHandshake101 =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    asio::ip::tcp::acceptor acceptor(
            ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto port = acceptor.local_endpoint().port();
    auto conn = std::make_shared<asio::ip::tcp::socket>(ioc);
    acceptor.async_accept(*conn, [conn](const std::error_code& ec) {
        if (ec) return;
        auto buf = std::make_shared<asio::streambuf>();
        asio::async_read_until(*conn, *buf, "\r\n\r\n",
            [conn, buf](const std::error_code& ec, size_t) {
                if (ec) return;
                asio::async_write(*conn, asio::buffer(kHandshake101.data(), kHandshake101.size()),
                    [conn, buf](const std::error_code& ec, size_t) {
                        if (ec) return;
                        // Pending read keeps the accepted socket (and buf) alive;
                        // without a pending handler the client sees an instant EOF
                        // instead of the idle timeout.
                        auto sink = std::make_shared<std::vector<char>>(64);
                        conn->async_read_some(asio::buffer(*sink),
                            [conn, sink](const std::error_code&, size_t) {});
                    });
            });
    });

    std::string script = R"(
        local http = require("http")
        local ws = http.ws_create("ws://127.0.0.1:__PORT__/echo", {idle_timeout = 200})
        local done = false
        ws.onmessage = function() end
        ws.onerror = function(err) done = true end
        ws.onclose = function() done = true end
        local ok = select(1, ws:connect())
        assert(ok == true, "expected connect to succeed")
        local waited = 0
        while not done and waited < 5000 do
            sleep(50)
            waited = waited + 50
        end
        assert(done, "idle timeout did not drop the connection within 5s")
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(port));

    const auto start = std::chrono::steady_clock::now();
    auto r = AWAIT(rt->RunScript(script));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();

    EXPECT_EQ(r.status, LUA_OK) << (r.status == LUA_OK ? "" : r.error);
    // Dropped by our 200ms idle timer (not an instant EOF, not a hang).
    EXPECT_GE(elapsed_ms, 150);
    EXPECT_LT(elapsed_ms, 5000);
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
