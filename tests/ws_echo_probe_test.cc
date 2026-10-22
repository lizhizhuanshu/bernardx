// Temporary probe: real ws echo server + Lua ws:send/onmessage round-trip.
// Reproduces the device hang where ws:connect succeeds but ws:send never resumes.
#include "http_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <asio.hpp>
#include <async_simple/coro/SyncAwait.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

namespace {

// Minimal RFC6455 server-side framing: reads one frame, echoes it back as text.
void serve_connection(std::shared_ptr<asio::ip::tcp::socket> sock) {
    auto buf = std::make_shared<std::vector<char>>(4096);
    auto pending = std::make_shared<std::string>();  // unparsed bytes
    // shared_ptr: the handler must outlive serve_connection's stack frame.
    auto read_some = std::make_shared<std::function<void(const std::error_code&, size_t)>>();
    *read_some = [sock, buf, pending, read_some](const std::error_code& ec, size_t n) mutable {
            if (ec) return;
            pending->append(buf->data(), n);
            // try to parse a complete frame
            while (pending->size() >= 2) {
                uint8_t b0 = (uint8_t)(*pending)[0];
                uint8_t b1 = (uint8_t)(*pending)[1];
                bool masked = b1 & 0x80;
                size_t len = b1 & 0x7f;
                size_t off = 2;
                if (len == 126) {
                    if (pending->size() < 4) return;
                    len = ((size_t)(uint8_t)(*pending)[2] << 8) | (uint8_t)(*pending)[3];
                    off = 4;
                } else if (len == 127) {
                    return;  // probe doesn't need huge frames
                }
                size_t mask_len = masked ? 4 : 0;
                if (pending->size() < off + mask_len + len) return;
                char mask[4] = {0};
                if (masked) memcpy(mask, pending->data() + off, 4);
                std::string payload = pending->substr(off + mask_len, len);
                if (masked) {
                    for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mask[i & 3];
                }
                pending->erase(0, off + mask_len + len);
                bool is_text = (b0 & 0x0f) == 0x1;
                bool is_close = (b0 & 0x0f) == 0x8;
                if (is_close) {
                    printf("[echo] got close frame, closing\n");
                    std::error_code sec;
                    sock->shutdown(asio::ip::tcp::socket::shutdown_both, sec);
                    return;
                }
                printf("[echo] got %s frame (%zu bytes): %.40s\n",
                       is_text ? "text" : "binary", payload.size(), payload.c_str());
                // echo: unmasked server frame
                std::string head;
                head.push_back((char)(0x80 | (is_text ? 0x1 : 0x2)));
                if (len < 126) {
                    head.push_back((char)len);
                } else {
                    head.push_back((char)126);
                    head.push_back((char)((len >> 8) & 0xff));
                    head.push_back((char)(len & 0xff));
                }
                auto out = std::make_shared<std::string>(head + payload);
                asio::async_write(*sock, asio::buffer(*out),
                                  [out](const std::error_code&, size_t) {});
            }
            sock->async_read_some(asio::buffer(*buf), *read_some);
        };
    sock->async_read_some(asio::buffer(*buf), *read_some);
}

}  // namespace

class WsEchoProbeTest : public ::testing::Test {
protected:
    void SetUp() override {
        work = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(ioc));
        exec = std::make_unique<coro_io::ExecutorWrapper<>>(ioc.get_executor());
        io_thread = std::thread([this]() { ioc.run(); });
        rt = LuaRuntime::Builder()
                .RegisterLibrary(std::make_shared<HttpLibrary>(*exec))
                .Create();

        static const std::string k101 =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
            "\r\n";
        acceptor = std::make_unique<asio::ip::tcp::acceptor>(
            ioc, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
        port = acceptor->local_endpoint().port();
        auto sock = std::make_shared<asio::ip::tcp::socket>(ioc);
        acceptor->async_accept(*sock, [sock](const std::error_code& ec) {
            ASSERT_FALSE(ec) << "accept: " << ec.message();
            auto buf = std::make_shared<asio::streambuf>();
            asio::async_read_until(*sock, *buf, "\r\n\r\n",
                [sock, buf](const std::error_code& ec, size_t) {
                    if (ec) return;
                    asio::async_write(*sock, asio::buffer(k101.data(), k101.size()),
                        [sock](const std::error_code& ec, size_t) {
                            if (ec) return;
                            serve_connection(sock);
                        });
                });
        });
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
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    uint16_t port = 0;
};

TEST_F(WsEchoProbeTest, SendReceivesEcho) {
    std::string script = R"(
        local http = require("http")
        local ws = http.ws_create("ws://127.0.0.1:__PORT__/echo")
        local got = {}
        ws.onmessage = function(m) got[#got + 1] = m end
        ws.onerror = function(e) print("!! onerror: " .. tostring(e)) end
        ws.onclose = function() print("!! onclose") end
        local ok, err = ws:connect()
        assert(ok, "connect: " .. tostring(err))
        print("connected")
        local sok, serr = ws:send("hello")
        print("send1 ->", sok, serr)
        assert(sok, "send failed: " .. tostring(serr))
        local waited = 0
        while #got < 1 and waited < 5000 do
            sleep(100)
            waited = waited + 100
        end
        print("got[1] =", tostring(got[1]))
        assert(got[1] == "hello", "echo mismatch: " .. tostring(got[1]))
        print("ECHO PASS")
    )";
    script.replace(script.find("__PORT__"), 8, std::to_string(port));

    auto r = AWAIT(rt->RunScript(script));
    EXPECT_EQ(r.status, LUA_OK) << r.error;
}
