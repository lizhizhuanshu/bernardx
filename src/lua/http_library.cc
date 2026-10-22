#include "http_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "library_state.h"
#include "lua_runtime.h"

#include <asio.hpp>
#include <cinatra/coro_http_client.hpp>

#include <async_simple/coro/Lazy.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

// --- Helpers ---

namespace {

LibraryState<HttpLibraryState> g_http_state{"http__state"};

static std::unordered_map<std::string, std::string> parse_headers(lua_State* L, int idx) {
    std::unordered_map<std::string, std::string> headers;
    if (lua_isnil(L, idx) || !lua_istable(L, idx)) {
        return headers;
    }
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
            headers.emplace(lua_tostring(L, -2), lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }
    return headers;
}

static cinatra::req_content_type parse_content_type(const char* s) {
    if (!s) return cinatra::req_content_type::none;
    if (strcmp(s, "json") == 0) return cinatra::req_content_type::json;
    if (strcmp(s, "text") == 0) return cinatra::req_content_type::text;
    if (strcmp(s, "html") == 0) return cinatra::req_content_type::html;
    if (strcmp(s, "xml") == 0) return cinatra::req_content_type::xml;
    if (strcmp(s, "form") == 0) return cinatra::req_content_type::form_url_encode;
    if (strcmp(s, "octet") == 0) return cinatra::req_content_type::octet_stream;
    return cinatra::req_content_type::none;
}

// --- Proxy ---

struct ProxyConfig {
    std::string host;
    std::string port;
    std::string username;
    std::string password;

    bool enabled() const { return !host.empty(); }
};

static void apply_proxy(cinatra::coro_http_client& client, const ProxyConfig& proxy) {
    if (!proxy.enabled()) return;
    client.set_proxy(proxy.host, proxy.port);
    if (!proxy.username.empty()) {
        client.set_proxy_basic_auth(proxy.username, proxy.password);
    }
}

// Parses "http://[user:pass@]host[:port]" or scheme-less "[user:pass@]host[:port]".
// Port defaults to 8080. Raises a Lua error (synchronous, before yield) on bad input.
static ProxyConfig parse_proxy(lua_State* L, const char* s) {
    ProxyConfig proxy;
    std::string_view sv(s);
    if (sv.empty()) {
        luaL_error(L, "http proxy must be a non-empty string");
        return proxy;  // unreachable (luaL_error longjmps)
    }

    // Optional scheme: only "http://" (or none). Anything else (socks5://, https://...)
    // is rejected — the underlying client speaks plain HTTP forward proxy only.
    auto scheme_end = sv.find("://");
    if (scheme_end != std::string_view::npos) {
        std::string_view scheme = sv.substr(0, scheme_end);
        if (scheme != "http") {
            // luaL_error's formatter only supports plain %s, not %.*s.
            luaL_error(L, "http proxy does not support scheme '%s' (only http)",
                       std::string(scheme).c_str());
            return proxy;
        }
        sv.remove_prefix(scheme_end + 3);
    }

    // Optional userinfo: everything before the last '@' is "user[:pass]".
    if (auto at = sv.rfind('@'); at != std::string_view::npos) {
        std::string_view userinfo = sv.substr(0, at);
        auto colon = userinfo.find(':');
        if (colon != std::string_view::npos) {
            proxy.username = std::string(userinfo.substr(0, colon));
            proxy.password = std::string(userinfo.substr(colon + 1));
        } else {
            proxy.username = std::string(userinfo);
        }
        sv.remove_prefix(at + 1);
    }

    // host[:port]; IPv6 literal hosts are bracketed ([::1]:8080).
    if (sv.starts_with('[')) {
        auto close = sv.find(']');
        if (close == std::string_view::npos) {
            luaL_error(L, "http proxy has unterminated IPv6 host in '%s'", s);
            return proxy;
        }
        proxy.host = std::string(sv.substr(1, close - 1));
        sv.remove_prefix(close + 1);
    } else {
        auto colon = sv.find(':');
        if (colon != std::string_view::npos) {
            proxy.host = std::string(sv.substr(0, colon));
            sv.remove_prefix(colon);
        } else {
            proxy.host = std::string(sv);
            sv = {};
        }
    }

    // Remaining ":port" (empty means default).
    if (!sv.empty()) {
        if (!sv.starts_with(':')) {
            luaL_error(L, "http proxy has invalid host/port in '%s'", s);
            return proxy;
        }
        sv.remove_prefix(1);
        if (!sv.empty() && sv.find_first_not_of("0123456789") == std::string_view::npos) {
            uint64_t port_val = 0;
            for (char c : sv) port_val = port_val * 10 + static_cast<uint64_t>(c - '0');
            if (port_val == 0 || port_val > 65535) {
                luaL_error(L, "http proxy port out of range in '%s'", s);
                return proxy;
            }
            proxy.port = std::string(sv);
        } else {
            luaL_error(L, "http proxy has invalid port in '%s'", s);
            return proxy;
        }
    }
    if (proxy.port.empty()) proxy.port = "8080";
    if (proxy.host.empty() || proxy.host.find('/') != std::string::npos) {
        luaL_error(L, "http proxy is missing or has invalid host in '%s'", s);
        return proxy;
    }
    return proxy;
}

// --- HTTP Lua C function ---

static int http_request(lua_State* L);

// --- Core HTTP request helper ---

// Runs the request Lazy built by `make_lazy` and resumes the yielded Lua
// coroutine with (status, body, err). The frame holds `state`, keeping the
// executor valid for the whole request.
template <typename F>
static async_simple::coro::Lazy<void> HttpRequestFlow(
    std::shared_ptr<HttpLibraryState> state, LuaRuntime::Ptr rt, AsyncHandle handle,
    F make_lazy) {
    auto client = std::make_shared<cinatra::coro_http_client>(state->exec);

    auto result = co_await make_lazy(*client).coAwaitTry();
    if (result.hasError()) {
        rt->PushResume(handle, {
            LuaValue{(int64_t)0},
            LuaValue{nullptr},
            LuaValue{std::string("internal error")}
        });
        co_return;
    }
    auto& resp = result.value();
    std::vector<LuaValue> values;
    values.emplace_back(static_cast<int64_t>(resp.status));
    if (resp.net_err) {
        values.emplace_back(nullptr);
        values.emplace_back(std::string(resp.net_err.message()));
    } else {
        values.emplace_back(std::string(resp.resp_body));
        values.emplace_back(nullptr);
    }
    rt->PushResume(handle, std::move(values));
}

template <typename F>
static int do_request(lua_State* L, F&& make_lazy) {
    auto state = g_http_state.Get(L);
    if (!state || state->shutting_down.load() || !state->exec) {
        return luaL_error(L, "http library is shutting down");
    }
    auto rt = LuaRuntime::FromLuaState(L);
    auto handle = rt->PreYield(L);
    auto* exec = state->exec;

    HttpRequestFlow(std::move(state), rt, handle, std::forward<F>(make_lazy))
        .via(exec)
        .detach();

    return rt->Yield(L);
}

enum class HttpMethod { kGet, kPost, kPut, kDelete };

// Case-insensitive method parsing; "GET" / nil / empty default to GET.
static HttpMethod parse_method(const char* s) {
    if (!s || !*s) return HttpMethod::kGet;
    std::string m(s);
    for (auto& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (m == "POST") return HttpMethod::kPost;
    if (m == "PUT") return HttpMethod::kPut;
    if (m == "DELETE") return HttpMethod::kDelete;
    return HttpMethod::kGet;  // "GET" and any unrecognized value
}

// http.request({url=, method=, body=, headers=, content_type=}) -> status, body, err
static int http_request(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "url");
    const char* url = lua_tostring(L, -1);
    if (!url) {
        return luaL_error(L, "http.request requires a string 'url'");
    }
    std::string url_str = url;
    lua_pop(L, 1);

    lua_getfield(L, 1, "method");
    const char* method = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
    HttpMethod http_method = parse_method(method);
    lua_pop(L, 1);

    lua_getfield(L, 1, "body");
    std::string body = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    lua_getfield(L, 1, "content_type");
    const char* ct = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
    cinatra::req_content_type content_type = parse_content_type(ct);
    lua_pop(L, 1);

    // timeout (optional, milliseconds; <= 0 leaves cinatra's 60s default)
    lua_getfield(L, 1, "timeout");
    int64_t timeout_ms = (lua_type(L, -1) == LUA_TNUMBER) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "headers");
    int headers_idx = lua_absindex(L, -1);
    auto headers = parse_headers(L, headers_idx);
    lua_pop(L, 1);

    // proxy (optional, "http://[user:pass@]host[:port]"; port defaults to 8080)
    lua_getfield(L, 1, "proxy");
    ProxyConfig proxy;
    if (const char* proxy_str = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr) {
        proxy = parse_proxy(L, proxy_str);
    }
    lua_pop(L, 1);

    return do_request(L, [url = std::move(url_str), body = std::move(body), content_type,
                          headers = std::move(headers), http_method,
                          timeout_ms, proxy](cinatra::coro_http_client& client) mutable {
        if (timeout_ms > 0) {
            client.set_req_timeout(std::chrono::milliseconds(timeout_ms));
        }
        apply_proxy(client, proxy);
        for (auto& [k, v] : headers) {
            client.add_header(k, v);
        }
        switch (http_method) {
            case HttpMethod::kPost:
                return client.async_post(url, std::move(body), content_type);
            case HttpMethod::kPut:
                return client.async_put(url, std::move(body), content_type);
            case HttpMethod::kDelete:
                return client.async_delete(url, std::move(body), content_type);
            case HttpMethod::kGet:
            default:
                return client.async_get(url);
        }
    });
}

// --- WebSocket ---
//
// WsConn is shared_ptr-managed so its lifetime extends beyond Lua userdata GC.
// The Lua userdata holds a shared_ptr via uservalue; the coroutine flows below
// capture the shared_ptr in their frames, ensuring the WsConn outlives any
// pending I/O.

static const char* kWsMetatable = "http__ws";

struct WsConn {
    std::shared_ptr<cinatra::coro_http_client> client;
    std::shared_ptr<HttpLibraryState> state;
    LuaRuntime::Ptr rt;
    std::string url;
    ProxyConfig proxy;
    std::atomic<bool> closed{false};

    // Timeout knobs (parsed on the Lua thread in ws_create, read on the io
    // thread). 0 keeps the engine default (30s handshake, 60s idle read).
    std::chrono::milliseconds connect_timeout{0};
    std::chrono::milliseconds idle_timeout{0};

    // Lua callback registry refs (accessed from Lua thread only)
    int on_message_ref = LUA_NOREF;
    int on_close_ref = LUA_NOREF;
    int on_error_ref = LUA_NOREF;

    // Unref all callback refs on the given state (Lua thread only)
    void UnrefCallbacks(lua_State* L) {
        if (on_message_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, on_message_ref); on_message_ref = LUA_NOREF; }
        if (on_close_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, on_close_ref); on_close_ref = LUA_NOREF; }
        if (on_error_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, on_error_ref); on_error_ref = LUA_NOREF; }
    }
};

// The userdata stores a shared_ptr<WsConn> directly.
static std::shared_ptr<WsConn> ws_get_conn(lua_State* L, int idx) {
    auto* ptr = static_cast<std::shared_ptr<WsConn>*>(luaL_testudata(L, idx, kWsMetatable));
    return ptr ? *ptr : nullptr;
}

static WsConn* ws_check(lua_State* L, int idx) {
    auto conn = ws_get_conn(L, idx);
    if (!conn) luaL_error(L, "invalid websocket");
    return conn.get();
}

// Defers dropping a coro_http_client reference: cinatra coroutine frames keep
// touching the client while they unwind after the completion callback returns
// (timer_guard destructors call timer_.cancel()), and coroutines aborted by
// close() resume on later io iterations. Releasing the last reference on the
// current stack corrupts the heap; posting the release lets every frame finish
// first.
static void defer_client_release(coro_io::ExecutorWrapper<>& exec,
                                 std::shared_ptr<cinatra::coro_http_client> client) {
    asio::post(exec.context(), [client = std::move(client)]() {});
}

// --- Coroutine flows ---
//
// Every ws Lua method yields, then drives a Lazy<void> flow on the io
// executor (`.via(exec).detach()`); the flow co_awaits cinatra client
// coroutines and resumes the Lua coroutine exactly once via PushResume.

// Resumes a yielded Lua coroutine with (ok, err); an empty err pushes nil.
static void WsResume(LuaRuntime::Ptr rt, AsyncHandle handle, bool ok,
                     std::string err = {}) {
    if (err.empty()) {
        rt->PushResume(handle, {LuaValue{ok}, LuaValue{nullptr}});
    } else {
        rt->PushResume(handle, {LuaValue{ok}, LuaValue{std::move(err)}});
    }
}

// Best-effort close-frame write (gc path). The frame holds the last client
// reference for the duration of the write — the write coroutine keeps only a
// raw `this` into the client.
static async_simple::coro::Lazy<void> WsCloseFrameFlow(
    std::shared_ptr<cinatra::coro_http_client> client, std::string reason) {
    co_await client->write_websocket_close(std::move(reason)).coAwaitTry();
}

// Unref callbacks on a specific lua_State.
// Called from the Lua event loop thread when the shared_ptr ref count drops.
static int ws_gc(lua_State* L) {
    auto* slot = static_cast<std::shared_ptr<WsConn>*>(luaL_testudata(L, 1, kWsMetatable));
    if (slot && *slot) {
        auto& conn = *slot;
        conn->UnrefCallbacks(L);
        auto client = std::move(conn->client);
        if (client && !conn->closed.exchange(true)) {
            auto state = conn->state;
            if (state && !state->shutting_down.load() && state->exec) {
                WsCloseFrameFlow(std::move(client), "gc").via(state->exec).detach();
            }
        }
        slot->reset();  // release shared_ptr (WsConn lives on if async callbacks still hold refs)
    }
    return 0;
}

// Optional integer field (table at `tbl`) in milliseconds; absent, non-number
// or <= 0 → 0, meaning "keep the engine default".
static std::chrono::milliseconds opt_timeout_ms(lua_State* L, int tbl, const char* field) {
    lua_getfield(L, tbl, field);
    lua_Integer v = (lua_type(L, -1) == LUA_TNUMBER) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    return std::chrono::milliseconds(v > 0 ? v : 0);
}

// http.ws_create(url [, options]) -> ws
// options.proxy: "http://[user:pass@]host[:port]" — route the websocket through a proxy.
// options.connect_timeout: handshake timeout in ms (0/absent → engine default 30s).
// options.idle_timeout: max silent time between inbound messages in ms (0/absent → default 60s).
static int ws_create(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    ProxyConfig proxy;
    std::chrono::milliseconds connect_timeout{0};
    std::chrono::milliseconds idle_timeout{0};
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "proxy");
        if (const char* proxy_str = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr) {
            proxy = parse_proxy(L, proxy_str);
        }
        lua_pop(L, 1);
        connect_timeout = opt_timeout_ms(L, 2, "connect_timeout");
        idle_timeout = opt_timeout_ms(L, 2, "idle_timeout");
    }
    auto state = g_http_state.Get(L);
    if (!state || state->shutting_down.load() || !state->exec) {
        return luaL_error(L, "http library is shutting down");
    }
    auto rt = LuaRuntime::FromLuaState(L);

    auto* slot = static_cast<std::shared_ptr<WsConn>*>(lua_newuserdatauv(L, sizeof(std::shared_ptr<WsConn>), 0));
    auto conn = std::make_shared<WsConn>();
    conn->state = std::move(state);
    conn->rt = rt;
    conn->url = url;
    conn->proxy = std::move(proxy);
    conn->connect_timeout = connect_timeout;
    conn->idle_timeout = idle_timeout;
    new (slot) std::shared_ptr<WsConn>(std::move(conn));
    luaL_setmetatable(L, kWsMetatable);

    return 1;
}

// ws.onmessage = fn / ws.onclose = fn / ws.onerror = fn
static int ws_newindex(lua_State* L) {
    auto* conn = ws_check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    int* slot = nullptr;
    if (strcmp(key, "onmessage") == 0) slot = &conn->on_message_ref;
    else if (strcmp(key, "onclose") == 0) slot = &conn->on_close_ref;
    else if (strcmp(key, "onerror") == 0) slot = &conn->on_error_ref;
    else luaL_error(L, "ws has no writable property '%s'", key);

    lua_pushvalue(L, 3);
    int old = *slot;
    *slot = luaL_ref(L, LUA_REGISTRYINDEX);
    if (old != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, old);
    return 0;
}

// ws.onmessage (read back)
static int ws_index(lua_State* L) {
    auto* conn = ws_check(L, 1);
    const char* key = lua_tostring(L, 2);

    if (key) {
        int ref = LUA_NOREF;
        if (strcmp(key, "onmessage") == 0) ref = conn->on_message_ref;
        else if (strcmp(key, "onclose") == 0) ref = conn->on_close_ref;
        else if (strcmp(key, "onerror") == 0) ref = conn->on_error_ref;

        if (ref != LUA_NOREF) {
            lua_geti(L, LUA_REGISTRYINDEX, ref);
            return 1;
        }
    }

    // Fall through to method table
    luaL_getmetatable(L, kWsMetatable);
    lua_getfield(L, -1, "__methods");
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    return 1;
}

// --- ws flows ---

// Background read pump. Dispatches onmessage/onclose/onerror until the
// connection drops or Lua closes it. The frame holds `conn`, which keeps the
// client alive for exactly as long as a read is pending.
static async_simple::coro::Lazy<void> WsReadLoop(std::shared_ptr<WsConn> conn) {
    auto* exec = conn->state->exec;
    auto& rt = conn->rt;
    spdlog::debug("http.ws read loop started: url={}", conn->url);
    while (!conn->closed.load()) {
        auto result = co_await conn->client->read_websocket().coAwaitTry();

        if (conn->closed.load()) {
            // Dropped by ws:close()/gc; the closer owns the client's teardown.
            // Hand off a reference (not the member) — a close flow racing us
            // still needs conn->client for its close-frame write.
            spdlog::debug("http.ws read loop stopped: closed by ws:close()/gc, url={}", conn->url);
            defer_client_release(*exec, conn->client);
            break;
        }

        auto& resp = result.value();
        if (result.hasError() || resp.net_err || resp.eof || resp.status != 200) {
            conn->closed.store(true);
            defer_client_release(*exec, std::move(conn->client));
            std::string err_msg = "connection closed";
            if (result.hasError()) {
                err_msg = "read failed";
            } else if (resp.net_err) {
                err_msg = std::string(resp.net_err.message());
            }
            spdlog::warn("http.ws read loop ended: url={} reason={} eof={} status={}",
                         conn->url, err_msg, resp.eof, resp.status);
            if (conn->on_error_ref != LUA_NOREF) {
                rt->CallLuaFunction(conn->on_error_ref, {std::move(err_msg)});
            } else if (conn->on_close_ref != LUA_NOREF) {
                rt->CallLuaFunction(conn->on_close_ref, {});
            }
            break;
        }

        // debug: carries a payload preview — debug builds only (release keeps
        // info-level lifecycle logs without message contents).
        spdlog::debug("http.ws recv: url={} bytes={} body={:.64}{}", conn->url,
                      resp.resp_body.size(), resp.resp_body,
                      conn->on_message_ref == LUA_NOREF ? " (no onmessage handler, dropped)" : "");
        if (conn->on_message_ref != LUA_NOREF) {
            rt->CallLuaFunction(conn->on_message_ref, {std::string(resp.resp_body)});
        }
    }
}

// ws:connect flow — performs the handshake, then spawns the read pump.
static async_simple::coro::Lazy<void> WsConnectFlow(std::shared_ptr<WsConn> conn,
                                                    LuaRuntime::Ptr rt, AsyncHandle handle) {
    auto* exec = conn->state->exec;
    auto client = std::make_shared<cinatra::coro_http_client>(exec);
    conn->client = client;
    apply_proxy(*client, conn->proxy);
    spdlog::debug("http.ws connect: url={} proxy={} connect_timeout={}ms idle_timeout={}ms",
                  conn->url, conn->proxy.host, conn->connect_timeout.count(),
                  conn->idle_timeout.count());

    // cinatra serializes every timer_guard onto the client's single timer, so
    // the handshake's inner "request timer" (60s default) silently overrides
    // the outer connect timer. Pin both to connect_timeout to bound the whole
    // handshake (TCP connect + upgrade); 0 keeps the engine defaults (30s/60s).
    if (conn->connect_timeout > std::chrono::milliseconds::zero()) {
        client->set_conn_timeout(conn->connect_timeout);
        client->set_req_timeout(conn->connect_timeout);
    }

    auto result = co_await client->connect(conn->url).coAwaitTry();

    if (conn->closed.load()) {
        // Aborted by ws:close()/gc while the handshake was in flight; the
        // closer already resumed Lua. Hand off a reference — a racing close
        // flow still needs conn->client.
        spdlog::debug("http.ws connect aborted by close during handshake: url={}", conn->url);
        defer_client_release(*exec, conn->client);
        co_return;
    }
    if (result.hasError() || result.value().net_err) {
        conn->closed.store(true);
        defer_client_release(*exec, std::move(conn->client));
        // Normalize the timeout error: errc::timed_out's message ("Connection
        // timed out") doesn't contain "timeout", which scripts match on.
        std::string msg = "connect failed";
        if (!result.hasError() && result.value().net_err == std::errc::timed_out) {
            msg = "connect timeout";
        } else if (!result.hasError()) {
            msg = std::string(result.value().net_err.message());
        }
        spdlog::warn("http.ws connect failed: url={} err={}", conn->url, msg);
        if (conn->on_error_ref != LUA_NOREF) {
            rt->CallLuaFunction(conn->on_error_ref, {msg});
        }
        WsResume(rt, handle, false, std::move(msg));
        co_return;
    }
    spdlog::info("http.ws connected: url={}", conn->url);

    // Re-pin the read pump's idle bound after the handshake (cinatra re-arms
    // the same timer per read_websocket call). Restore the 60s engine default
    // when the handshake shortened req_timeout and no idle_timeout was given.
    if (conn->idle_timeout > std::chrono::milliseconds::zero()) {
        client->set_req_timeout(conn->idle_timeout);
    } else if (conn->connect_timeout > std::chrono::milliseconds::zero()) {
        client->set_req_timeout(std::chrono::seconds(60));
    }

    WsReadLoop(conn).via(exec).detach();

    WsResume(rt, handle, true);
}

// ws:send flow.
static async_simple::coro::Lazy<void> WsSendFlow(std::shared_ptr<WsConn> conn, LuaRuntime::Ptr rt,
                                                 AsyncHandle handle, std::string msg,
                                                 cinatra::opcode op) {
    if (!conn->client || conn->closed.load()) {
        auto reason = conn->client ? "closed" : "not connected";
        spdlog::debug("http.ws send rejected: url={} bytes={} reason={}",
                      conn->url, msg.size(), reason);
        WsResume(rt, handle, false, reason);
        co_return;
    }
    // The frame holds a client reference for the duration of the write.
    auto client = conn->client;
    auto result = co_await client->write_websocket(msg, op).coAwaitTry();
    bool ok = !result.hasError() && !result.value().net_err;
    if (!ok) {
        spdlog::warn("http.ws send failed: url={} bytes={} err={}", conn->url, msg.size(),
                     result.hasError() ? "write coroutine failed"
                                       : result.value().net_err.message());
    }
    WsResume(rt, handle, ok, ok ? std::string{} : std::string{"send failed"});
}

// ws:close flow — writes the close frame, then resumes.
static async_simple::coro::Lazy<void> WsCloseFlow(std::shared_ptr<WsConn> conn, LuaRuntime::Ptr rt,
                                                  AsyncHandle handle) {
    if (conn->client) {
        // The frame holds `conn`, keeping the client alive for the write.
        spdlog::debug("http.ws close: url={}", conn->url);
        auto result = co_await conn->client->write_websocket_close("bye").coAwaitTry();
        if (result.hasError() || result.value().net_err) {
            spdlog::warn("http.ws close frame write failed: url={} err={}", conn->url,
                         result.hasError() ? "write coroutine failed"
                                           : result.value().net_err.message());
        }
    }
    WsResume(rt, handle, true);
}

// --- ws Lua entry points ---

// ws:connect() -> ok, err  (yields until handshake done)
static int ws_connect_method(lua_State* L) {
    auto* conn = ws_check(L, 1);
    if (conn->closed.load()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "closed");
        return 2;
    }
    if (!conn->state || conn->state->shutting_down.load() || !conn->state->exec) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "http library is shutting down");
        return 2;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto handle = rt->PreYield(L);
    WsConnectFlow(ws_get_conn(L, 1), rt, handle).via(conn->state->exec).detach();

    return rt->Yield(L);
}

// ws:send(msg [, "binary"]) -> true, err
static int ws_send(lua_State* L) {
    auto* conn = ws_check(L, 1);
    if (conn->closed.load()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "closed");
        return 2;
    }
    if (!conn->state || conn->state->shutting_down.load() || !conn->state->exec) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "http library is shutting down");
        return 2;
    }
    size_t len;
    const char* msg = luaL_checklstring(L, 2, &len);
    const char* mode = lua_isstring(L, 3) ? lua_tostring(L, 3) : "text";
    auto op = (mode && mode[0] == 'b') ? cinatra::opcode::binary : cinatra::opcode::text;

    auto rt = LuaRuntime::FromLuaState(L);
    auto handle = rt->PreYield(L);
    WsSendFlow(ws_get_conn(L, 1), rt, handle, std::string(msg, len), op)
        .via(conn->state->exec)
        .detach();

    return rt->Yield(L);
}

// ws:close() -> ok, err
static int ws_close(lua_State* L) {
    auto* conn = ws_check(L, 1);
    if (conn->closed.exchange(true)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (!conn->state || conn->state->shutting_down.load() || !conn->state->exec) {
        lua_pushboolean(L, 1);
        return 1;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto handle = rt->PreYield(L);
    WsCloseFlow(ws_get_conn(L, 1), rt, handle).via(conn->state->exec).detach();

    return rt->Yield(L);
}

static const luaL_Reg ws_methods[] = {
    {"connect", ws_connect_method},
    {"send", ws_send},
    {"close", ws_close},
    {nullptr, nullptr}
};

}  // namespace

// --- HttpLibrary ---

HttpLibrary::HttpLibrary(coro_io::ExecutorWrapper<>& exec) : exec_(exec) {}

HttpLibrary::~HttpLibrary() {
    Close(nullptr);
}

void HttpLibrary::Open(lua_State* L) {
    // Register ws metatable (idempotent)
    if (luaL_newmetatable(L, kWsMetatable)) {
        lua_pushcfunction(L, ws_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, ws_newindex);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, ws_index);
        lua_setfield(L, -2, "__index");
        luaL_newlib(L, ws_methods);
        lua_setfield(L, -2, "__methods");
    }
    lua_pop(L, 1);

    // Create http table
    lua_newtable(L);
    luaL_Reg funcs[] = {
        {"request", http_request},
        {"ws_create", ws_create},
        {nullptr, nullptr}
    };
    auto state = std::make_shared<HttpLibraryState>();
    state->exec = &exec_;
    g_http_state.Set(L, std::move(state));
    luaL_setfuncs(L, funcs, 0);
}

void HttpLibrary::Close(lua_State* L) {
    if (!L) return;
    auto state = g_http_state.Get(L);
    if (!state || state->shutting_down.exchange(true)) return;
}
