#include "http_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_runtime.h"

#include <asio.hpp>
#include <cinatra/coro_http_client.hpp>

#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// --- Helpers ---

namespace {

int kHttpStateRegistryKey = 0;
constexpr const char* kHttpStateMetatable = "http__state";

int http_state_gc(lua_State* L) {
    auto* slot = static_cast<std::shared_ptr<HttpLibraryState>*>(lua_touserdata(L, 1));
    if (slot) {
        slot->~shared_ptr<HttpLibraryState>();
    }
    return 0;
}

void EnsureHttpStateMetatable(lua_State* L) {
    if (luaL_newmetatable(L, kHttpStateMetatable)) {
        lua_pushcfunction(L, http_state_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

void SetHttpState(lua_State* L, std::shared_ptr<HttpLibraryState> state) {
    EnsureHttpStateMetatable(L);
    auto* slot = static_cast<std::shared_ptr<HttpLibraryState>*>(
        lua_newuserdatauv(L, sizeof(std::shared_ptr<HttpLibraryState>), 0));
    new (slot) std::shared_ptr<HttpLibraryState>(std::move(state));
    luaL_setmetatable(L, kHttpStateMetatable);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kHttpStateRegistryKey);
}

std::shared_ptr<HttpLibraryState> GetHttpState(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kHttpStateRegistryKey);
    auto* slot = static_cast<std::shared_ptr<HttpLibraryState>*>(lua_touserdata(L, -1));
    auto state = slot ? *slot : nullptr;
    lua_pop(L, 1);
    return state;
}

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

// --- HTTP Lua C functions ---

static int http_get(lua_State* L);
static int http_post(lua_State* L);
static int http_put(lua_State* L);
static int http_del(lua_State* L);

// --- Core HTTP request helper ---

template <typename F>
static int http_request(lua_State* L, F&& make_lazy) {
    auto state = GetHttpState(L);
    if (!state || state->shutting_down.load() || !state->exec) {
        return luaL_error(L, "http library is shutting down");
    }
    auto* exec = state->exec;
    auto rt = LuaRuntime::FromLuaState(L);
    auto handle = rt->PreYield(L);

    asio::post(exec->context(), [state, exec, rt, handle, lazy_factory = std::forward<F>(make_lazy)]() mutable {
        auto client = std::make_shared<cinatra::coro_http_client>(exec);

        auto lazy = lazy_factory(*client);
        std::move(lazy).via(exec).start([state, rt, handle, client](async_simple::Try<cinatra::resp_data>&& result) {
            if (result.hasError()) {
                rt->PushResume(handle, {
                    LuaValue{(int64_t)0},
                    LuaValue{nullptr},
                    LuaValue{std::string("internal error")}
                });
                return;
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
        });
    });

    return rt->Yield(L);
}

static int http_get(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    auto headers = parse_headers(L, 2);
    return http_request(L, [url = std::string(url), headers = std::move(headers)](cinatra::coro_http_client& client) {
        for (auto& [k, v] : headers) {
            client.add_header(k, v);
        }
        return client.async_get(url);
    });
}

static int http_post(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    std::string body = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const char* ct = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;
    auto headers = parse_headers(L, 4);
    auto content_type = parse_content_type(ct);
    return http_request(L, [url = std::string(url), body = std::move(body), content_type, headers = std::move(headers)](cinatra::coro_http_client& client) mutable {
        for (auto& [k, v] : headers) {
            client.add_header(k, v);
        }
        return client.async_post(url, std::move(body), content_type);
    });
}

static int http_put(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    std::string body = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const char* ct = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;
    auto headers = parse_headers(L, 4);
    auto content_type = parse_content_type(ct);
    return http_request(L, [url = std::string(url), body = std::move(body), content_type, headers = std::move(headers)](cinatra::coro_http_client& client) mutable {
        for (auto& [k, v] : headers) {
            client.add_header(k, v);
        }
        return client.async_put(url, std::move(body), content_type);
    });
}

static int http_del(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    auto headers = parse_headers(L, 2);
    return http_request(L, [url = std::string(url), headers = std::move(headers)](cinatra::coro_http_client& client) {
        for (auto& [k, v] : headers) {
            client.add_header(k, v);
        }
        return client.async_delete(url, "", cinatra::req_content_type::none);
    });
}

// --- WebSocket ---
//
// WsConn is shared_ptr-managed so its lifetime extends beyond Lua userdata GC.
// The Lua userdata holds a shared_ptr via uservalue; all async callbacks capture
// the shared_ptr, ensuring the WsConn outlives any pending I/O.

static const char* kWsMetatable = "http__ws";

struct WsConn {
    std::shared_ptr<cinatra::coro_http_client> client;
    std::shared_ptr<HttpLibraryState> state;
    LuaRuntime::Ptr rt;
    std::string url;
    std::atomic<bool> closed{false};

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
                auto* exec = state->exec;
                asio::post(exec->context(), [state, client, exec] {
                    client->write_websocket_close("gc").via(exec).start([](auto&&) {});
                });
            }
        }
        slot->reset();  // release shared_ptr (WsConn lives on if async callbacks still hold refs)
    }
    return 0;
}

// http.ws_create(url) -> ws
static int ws_create(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    auto state = GetHttpState(L);
    if (!state || state->shutting_down.load() || !state->exec) {
        return luaL_error(L, "http library is shutting down");
    }
    auto rt = LuaRuntime::FromLuaState(L);

    auto* slot = static_cast<std::shared_ptr<WsConn>*>(lua_newuserdatauv(L, sizeof(std::shared_ptr<WsConn>), 0));
    auto conn = std::make_shared<WsConn>();
    conn->state = std::move(state);
    conn->rt = rt;
    conn->url = url;
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
    auto* exec = conn->state->exec;
    // Capture shared_ptr to keep WsConn alive for the duration of async ops
    auto shared_conn = ws_get_conn(L, 1);

    asio::post(exec->context(), [exec, rt, handle, shared_conn]() mutable {
        auto client = std::make_shared<cinatra::coro_http_client>(exec);
        shared_conn->client = client;

        client->connect(shared_conn->url).via(exec).start([rt, handle, shared_conn, exec](async_simple::Try<cinatra::resp_data>&& result) {
            if (shared_conn->closed.load()) return;
            if (result.hasError() || result.value().net_err) {
                shared_conn->closed.store(true);
                shared_conn->client.reset();
                auto msg = result.hasError()
                    ? std::string("connect failed")
                    : std::string(result.value().net_err.message());
                if (shared_conn->on_error_ref != LUA_NOREF)
                    rt->CallLuaFunction(shared_conn->on_error_ref, {std::move(msg)});
                rt->PushResume(handle, {LuaValue{false}, LuaValue{nullptr}});
                return;
            }

            // Start background read loop (captures shared_conn by value)
            auto do_read = std::make_shared<std::function<void()>>();
            *do_read = [exec, shared_conn, do_read]() {
                shared_conn->client->read_websocket().via(exec).start([exec, shared_conn, do_read](async_simple::Try<cinatra::resp_data>&& result) {
                    if (shared_conn->closed.load()) return;
                    if (result.hasError() || result.value().net_err || result.value().eof || result.value().status != 200) {
                        shared_conn->closed.store(true);
                        shared_conn->client.reset();
                        auto err_msg = result.hasError() ? std::string("read failed")
                            : result.value().net_err ? std::string(result.value().net_err.message())
                            : std::string("connection closed");
                        if (shared_conn->on_error_ref != LUA_NOREF)
                            shared_conn->rt->CallLuaFunction(shared_conn->on_error_ref, {std::move(err_msg)});
                        else if (shared_conn->on_close_ref != LUA_NOREF)
                            shared_conn->rt->CallLuaFunction(shared_conn->on_close_ref, {});
                        return;
                    }
                    if (shared_conn->on_message_ref != LUA_NOREF) {
                        shared_conn->rt->CallLuaFunction(shared_conn->on_message_ref, {std::string(result.value().resp_body)});
                    }
                    // Schedule next read
                    asio::post(exec->context(), *do_read);
                });
            };
            (*do_read)();

            rt->PushResume(handle, {LuaValue{true}, LuaValue{nullptr}});
        });
    });

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
    auto* exec = conn->state->exec;
    auto client = conn->client;
    auto shared_conn = ws_get_conn(L, 1);

    asio::post(exec->context(), [exec, rt, handle, client, shared_conn, msg = std::string(msg, len), op]() mutable {
        if (shared_conn->closed.load()) {
            rt->PushResume(handle, {LuaValue{false}, LuaValue{std::string("closed")}});
            return;
        }
        client->write_websocket(msg, op).via(exec).start(
            [state = shared_conn->state, rt, handle](async_simple::Try<cinatra::resp_data>&& result) {
            if (result.hasError() || result.value().net_err) {
                rt->PushResume(handle, {LuaValue{false}, LuaValue{std::string("send failed")}});
            } else {
                rt->PushResume(handle, {LuaValue{true}, LuaValue{nullptr}});
            }
        });
    });

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
    auto* exec = conn->state->exec;
    auto client = conn->client;
    auto state = conn->state;

    asio::post(exec->context(), [state, exec, rt, handle, client]() mutable {
        client->write_websocket_close("bye").via(exec).start([state, rt, handle](async_simple::Try<cinatra::resp_data>&&) {
            rt->PushResume(handle, {LuaValue{true}, LuaValue{nullptr}});
        });
    });

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
        {"get", http_get},
        {"post", http_post},
        {"put", http_put},
        {"del", http_del},
        {"ws_create", ws_create},
        {nullptr, nullptr}
    };
    auto state = std::make_shared<HttpLibraryState>();
    state->exec = &exec_;
    SetHttpState(L, state);
    luaL_setfuncs(L, funcs, 0);
}

void HttpLibrary::Close(lua_State* L) {
    if (!L) return;
    auto state = GetHttpState(L);
    if (!state || state->shutting_down.exchange(true)) return;
}
