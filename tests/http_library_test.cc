#include "http_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

// --- HttpLibrary ---

class HttpLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        rt = LuaRuntime::Builder()
            .RegisterLibrary(std::make_shared<HttpLibrary>())
            .Create();
    }

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
        -- connection refused: status may be 0 or a non-200 code, err should be non-nil
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
