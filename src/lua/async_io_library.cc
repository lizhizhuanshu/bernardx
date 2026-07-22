#include "async_io_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "library_state.h"
#include "lua_runtime.h"

#include <asio.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// --- State management ---

namespace {

constexpr const char* kFileMetatable = "async__file";
constexpr const char* kProcessMetatable = "async__process";

LibraryState<AsyncIOState> g_async_state{"async__state"};

// --- Handle types ---

struct FileHandle {
    std::shared_ptr<AsyncIOState> state;
    std::optional<asio::posix::stream_descriptor> sd;
    std::atomic<bool> closed{false};
    std::string path;
    std::string read_buffer;
};

struct ProcessHandle {
    std::shared_ptr<AsyncIOState> state;
    pid_t pid = -1;
    std::optional<asio::posix::stream_descriptor> stdin_sd;
    std::optional<asio::posix::stream_descriptor> stdout_sd;
    std::atomic<bool> closed{false};
    std::string cmd;
    std::string read_buffer;
};

// --- Helpers ---

static std::shared_ptr<FileHandle> file_get(lua_State* L, int idx) {
    auto* slot = static_cast<std::shared_ptr<FileHandle>*>(
        luaL_testudata(L, idx, kFileMetatable));
    return slot ? *slot : nullptr;
}

static std::shared_ptr<FileHandle> file_check(lua_State* L, int idx) {
    auto h = file_get(L, idx);
    if (!h) luaL_error(L, "expected async file handle");
    return h;
}

static std::shared_ptr<ProcessHandle> process_get(lua_State* L, int idx) {
    auto* slot = static_cast<std::shared_ptr<ProcessHandle>*>(
        luaL_testudata(L, idx, kProcessMetatable));
    return slot ? *slot : nullptr;
}

static std::shared_ptr<ProcessHandle> process_check(lua_State* L, int idx) {
    auto h = process_get(L, idx);
    if (!h) luaL_error(L, "expected async process handle");
    return h;
}

enum ReadMode { READ_ALL, READ_LINE, READ_BYTES };

struct ReadSpec {
    ReadMode mode;
    size_t byte_count = 0;
};

static ReadSpec parse_read_fmt(lua_State* L, int arg) {
    ReadSpec spec{READ_LINE, 0};
    if (lua_isnoneornil(L, arg)) return spec;
    if (lua_type(L, arg) == LUA_TNUMBER) {
        spec.mode = READ_BYTES;
        spec.byte_count = static_cast<size_t>(luaL_checkinteger(L, arg));
        return spec;
    }
    const char* fmt = luaL_checkstring(L, arg);
    if (strcmp(fmt, "*a") == 0) spec.mode = READ_ALL;
    else if (strcmp(fmt, "*l") == 0) spec.mode = READ_LINE;
    else luaL_error(L, "invalid read format '%s'", fmt);
    return spec;
}

static std::string strip_trailing_newline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

// --- Async line reader (recursive async_read_some on io_context thread) ---

template <typename HandleT>
void do_read_line(std::shared_ptr<HandleT> handle,
                  asio::posix::stream_descriptor& sd,
                  LuaRuntime::Ptr rt, AsyncHandle yield_handle) {
    auto pos = handle->read_buffer.find('\n');
    if (pos != std::string::npos) {
        std::string line = handle->read_buffer.substr(0, pos);
        handle->read_buffer.erase(0, pos + 1);
        line = strip_trailing_newline(std::move(line));
        rt->PushResume(yield_handle, {LuaValue{std::move(line)}});
        return;
    }

    auto buf = std::make_shared<std::array<char, 4096>>();
    sd.async_read_some(asio::buffer(*buf),
        [&sd, handle, rt, yield_handle, buf](asio::error_code ec, size_t bytes) mutable {
            if (handle->closed.load()) return;

            if (ec || bytes == 0) {
                auto line = strip_trailing_newline(std::move(handle->read_buffer));
                handle->read_buffer.clear();
                if (line.empty()) {
                    rt->PushResume(yield_handle, {LuaValue{nullptr}});
                } else {
                    rt->PushResume(yield_handle, {LuaValue{std::move(line)}});
                }
                return;
            }

            handle->read_buffer.append(buf->data(), bytes);
            auto p = handle->read_buffer.find('\n');
            if (p != std::string::npos) {
                std::string line = handle->read_buffer.substr(0, p);
                handle->read_buffer.erase(0, p + 1);
                line = strip_trailing_newline(std::move(line));
                rt->PushResume(yield_handle, {LuaValue{std::move(line)}});
            } else {
                do_read_line(handle, sd, rt, yield_handle);
            }
        });
}

// --- FileHandle GC / index ---

static int file_gc(lua_State* L) {
    auto* slot = static_cast<std::shared_ptr<FileHandle>*>(
        luaL_testudata(L, 1, kFileMetatable));
    if (slot && *slot) {
        auto& h = *slot;
        if (!h->closed.exchange(true)) {
            if (h->sd.has_value()) h->sd->close();
            h->sd.reset();
        }
        slot->~shared_ptr<FileHandle>();
    }
    return 0;
}

static int file_index(lua_State* L) {
    luaL_checkudata(L, 1, kFileMetatable);
    luaL_getmetatable(L, kFileMetatable);
    lua_getfield(L, -1, "__methods");
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    return 1;
}

// --- FileHandle methods ---

static int file_read(lua_State* L) {
    auto handle = file_check(L, 1);
    auto spec = parse_read_fmt(L, 2);

    if (handle->closed.load() || !handle->sd.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "file is closed");
        return 2;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto yield_handle = rt->PreYield(L);
    auto shared_h = file_get(L, 1);
    auto& sd = shared_h->sd.value();

    switch (spec.mode) {
        case READ_ALL: {
            auto buf = std::make_shared<std::string>(std::move(shared_h->read_buffer));
            shared_h->read_buffer.clear();
            asio::async_read(sd, asio::dynamic_buffer(*buf),
                [shared_h, rt, yield_handle, buf](asio::error_code, size_t) mutable {
                    if (shared_h->closed.load()) return;
                    if (buf->empty()) {
                        rt->PushResume(yield_handle, {LuaValue{nullptr}});
                    } else {
                        rt->PushResume(yield_handle, {LuaValue{std::move(*buf)}});
                    }
                });
            break;
        }
        case READ_LINE: {
            do_read_line(shared_h, sd, rt, yield_handle);
            break;
        }
        case READ_BYTES: {
            if (spec.byte_count == 0) {
                rt->PushResume(yield_handle, {LuaValue{std::string{}}});
                break;
            }
            auto buf = std::make_shared<std::string>();
            auto remaining = spec.byte_count;
            if (shared_h->read_buffer.size() >= remaining) {
                *buf = shared_h->read_buffer.substr(0, remaining);
                shared_h->read_buffer.erase(0, remaining);
                rt->PushResume(yield_handle, {LuaValue{std::move(*buf)}});
                break;
            }
            *buf = std::move(shared_h->read_buffer);
            shared_h->read_buffer.clear();
            remaining -= buf->size();
            buf->resize(spec.byte_count);
            asio::async_read(sd, asio::buffer(buf->data() + buf->size() - remaining, remaining),
                [shared_h, rt, yield_handle, buf, remaining](asio::error_code ec, size_t bytes) mutable {
                    if (shared_h->closed.load()) return;
                    auto total = buf->size() - remaining + bytes;
                    buf->resize(total);
                    if (total == 0) {
                        rt->PushResume(yield_handle, {LuaValue{nullptr}});
                    } else {
                        rt->PushResume(yield_handle, {LuaValue{std::move(*buf)}});
                    }
                });
            break;
        }
    }

    return rt->Yield(L);
}

static int file_write(lua_State* L) {
    auto handle = file_check(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);

    if (handle->closed.load() || !handle->sd.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "file is closed");
        return 2;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto yield_handle = rt->PreYield(L);
    auto shared_h = file_get(L, 1);
    auto buf = std::make_shared<std::string>(data, len);

    asio::async_write(*shared_h->sd, asio::buffer(*buf),
        [shared_h, rt, yield_handle, buf](asio::error_code ec, size_t) {
            if (shared_h->closed.load()) return;
            if (ec) {
                rt->PushResume(yield_handle, {
                    LuaValue{nullptr},
                    LuaValue{"write error: " + shared_h->path}
                });
            } else {
                rt->PushResume(yield_handle, {LuaValue{true}});
            }
        });

    return rt->Yield(L);
}

static int file_close(lua_State* L) {
    auto handle = file_check(L, 1);
    if (handle->closed.exchange(true)) {
        lua_pushboolean(L, true);
        return 1;
    }
    if (handle->sd.has_value()) {
        handle->sd->close();
        handle->sd.reset();
    }
    handle->read_buffer.clear();
    lua_pushboolean(L, true);
    return 1;
}

static const luaL_Reg file_methods[] = {
    {"read", file_read},
    {"write", file_write},
    {"close", file_close},
    {nullptr, nullptr}
};

// --- ProcessHandle GC / index ---

static int process_gc(lua_State* L) {
    auto* slot = static_cast<std::shared_ptr<ProcessHandle>*>(
        luaL_testudata(L, 1, kProcessMetatable));
    if (slot && *slot) {
        auto& h = *slot;
        if (!h->closed.exchange(true)) {
            if (h->stdin_sd.has_value()) { h->stdin_sd->close(); h->stdin_sd.reset(); }
            if (h->stdout_sd.has_value()) { h->stdout_sd->close(); h->stdout_sd.reset(); }
            if (h->pid > 0) {
                kill(h->pid, SIGTERM);
                waitpid(h->pid, nullptr, WNOHANG);
                h->pid = -1;
            }
        }
        slot->~shared_ptr<ProcessHandle>();
    }
    return 0;
}

static int process_index(lua_State* L) {
    luaL_checkudata(L, 1, kProcessMetatable);
    luaL_getmetatable(L, kProcessMetatable);
    lua_getfield(L, -1, "__methods");
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    return 1;
}

// --- ProcessHandle methods ---

static int process_read(lua_State* L) {
    auto handle = process_check(L, 1);
    auto spec = parse_read_fmt(L, 2);

    if (handle->closed.load() || !handle->stdout_sd.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "process is closed");
        return 2;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto yield_handle = rt->PreYield(L);
    auto shared_h = process_get(L, 1);
    auto& sd = shared_h->stdout_sd.value();

    switch (spec.mode) {
        case READ_ALL: {
            auto buf = std::make_shared<std::string>(std::move(shared_h->read_buffer));
            shared_h->read_buffer.clear();
            asio::async_read(sd, asio::dynamic_buffer(*buf),
                [shared_h, rt, yield_handle, buf](asio::error_code, size_t) mutable {
                    if (shared_h->closed.load()) return;
                    if (buf->empty()) {
                        rt->PushResume(yield_handle, {LuaValue{nullptr}});
                    } else {
                        rt->PushResume(yield_handle, {LuaValue{std::move(*buf)}});
                    }
                });
            break;
        }
        case READ_LINE: {
            do_read_line(shared_h, sd, rt, yield_handle);
            break;
        }
        case READ_BYTES: {
            if (spec.byte_count == 0) {
                rt->PushResume(yield_handle, {LuaValue{std::string{}}});
                break;
            }
            auto buf = std::make_shared<std::string>();
            auto remaining = spec.byte_count;
            if (shared_h->read_buffer.size() >= remaining) {
                *buf = shared_h->read_buffer.substr(0, remaining);
                shared_h->read_buffer.erase(0, remaining);
                rt->PushResume(yield_handle, {LuaValue{std::move(*buf)}});
                break;
            }
            *buf = std::move(shared_h->read_buffer);
            shared_h->read_buffer.clear();
            remaining -= buf->size();
            buf->resize(spec.byte_count);
            asio::async_read(sd, asio::buffer(buf->data() + buf->size() - remaining, remaining),
                [shared_h, rt, yield_handle, buf, remaining](asio::error_code ec, size_t bytes) mutable {
                    if (shared_h->closed.load()) return;
                    auto total = buf->size() - remaining + bytes;
                    buf->resize(total);
                    if (total == 0) {
                        rt->PushResume(yield_handle, {LuaValue{nullptr}});
                    } else {
                        rt->PushResume(yield_handle, {LuaValue{std::move(*buf)}});
                    }
                });
            break;
        }
    }

    return rt->Yield(L);
}

static int process_write(lua_State* L) {
    auto handle = process_check(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);

    if (handle->closed.load() || !handle->stdin_sd.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "process stdin is closed");
        return 2;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto yield_handle = rt->PreYield(L);
    auto shared_h = process_get(L, 1);
    auto buf = std::make_shared<std::string>(data, len);

    asio::async_write(*shared_h->stdin_sd, asio::buffer(*buf),
        [shared_h, rt, yield_handle, buf](asio::error_code ec, size_t) {
            if (shared_h->closed.load()) return;
            if (ec) {
                auto msg = (ec == asio::error::broken_pipe)
                    ? std::string("broken pipe")
                    : std::string("write error: ") + ec.message();
                rt->PushResume(yield_handle, {
                    LuaValue{nullptr},
                    LuaValue{std::move(msg)}
                });
            } else {
                rt->PushResume(yield_handle, {LuaValue{true}});
            }
        });

    return rt->Yield(L);
}

static int process_kill(lua_State* L) {
    auto handle = process_check(L, 1);
    int sig = luaL_optinteger(L, 2, SIGTERM);

    if (handle->closed.load()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "process is closed");
        return 2;
    }

    if (handle->pid <= 0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "no process to kill");
        return 2;
    }

    if (::kill(handle->pid, sig) < 0) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "kill failed: %s", strerror(errno));
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

static int process_shutdown(lua_State* L) {
    auto handle = process_check(L, 1);
    if (handle->stdin_sd.has_value()) {
        handle->stdin_sd->close();
        handle->stdin_sd.reset();
    }
    lua_pushboolean(L, true);
    return 1;
}

static int process_close(lua_State* L) {
    auto handle = process_check(L, 1);
    if (handle->closed.exchange(true)) {
        lua_pushinteger(L, 0);
        return 1;
    }

    if (handle->stdin_sd.has_value()) { handle->stdin_sd->close(); handle->stdin_sd.reset(); }
    if (handle->stdout_sd.has_value()) { handle->stdout_sd->close(); handle->stdout_sd.reset(); }
    handle->read_buffer.clear();

    if (handle->pid <= 0) {
        lua_pushinteger(L, 0);
        return 1;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto yield_handle = rt->PreYield(L);
    auto shared_h = process_get(L, 1);
    auto pid = shared_h->pid;

    asio::post(shared_h->state->ioc.get(),
        [rt, yield_handle, shared_h, pid]() mutable {
            int status = 0;
            int rc = waitpid(pid, &status, 0);
            shared_h->pid = -1;

            if (rc < 0) {
                rt->PushResume(yield_handle, {
                    LuaValue{(int64_t)-1},
                    LuaValue{std::string("waitpid failed: ") + strerror(errno)}
                });
                return;
            }

            int exit_code = -1;
            if (WIFEXITED(status))
                exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                exit_code = -WTERMSIG(status);
            rt->PushResume(yield_handle, {LuaValue{(int64_t)exit_code}});
        });

    return rt->Yield(L);
}

static const luaL_Reg process_methods[] = {
    {"read", process_read},
    {"write", process_write},
    {"shutdown", process_shutdown},
    {"close", process_close},
    {"kill", process_kill},
    {nullptr, nullptr}
};

// --- Module functions ---

static int async_open(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const char* mode = luaL_optstring(L, 2, "r");

    auto state = g_async_state.Get(L);
    if (!state || state->shutting_down.load()) {
        return luaL_error(L, "async library is shutting down");
    }

    int flags = O_RDONLY;
    if (strcmp(mode, "r") == 0) flags = O_RDONLY;
    else if (strcmp(mode, "w") == 0) flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (strcmp(mode, "a") == 0) flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (strcmp(mode, "r+") == 0) flags = O_RDWR;
    else if (strcmp(mode, "w+") == 0) flags = O_RDWR | O_CREAT | O_TRUNC;
    else { return luaL_error(L, "invalid mode '%s'", mode); }

    int fd = open(path, flags, 0644);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open file '%s': %s", path, strerror(errno));
        return 2;
    }

    auto* slot = static_cast<std::shared_ptr<FileHandle>*>(
        lua_newuserdatauv(L, sizeof(std::shared_ptr<FileHandle>), 0));
    auto handle = std::make_shared<FileHandle>();
    handle->state = state;
    handle->path = path;
    handle->sd.emplace(state->ioc.get(), fd);
    new (slot) std::shared_ptr<FileHandle>(std::move(handle));
    luaL_setmetatable(L, kFileMetatable);
    return 1;
}

static int async_exec(lua_State* L) {
    const char* cmd = luaL_checkstring(L, 1);

    auto state = g_async_state.Get(L);
    if (!state || state->shutting_down.load()) {
        return luaL_error(L, "async library is shutting down");
    }

    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "pipe creation failed");
        return 2;
    }
    if (pipe(stdout_pipe) < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        lua_pushnil(L);
        lua_pushstring(L, "pipe creation failed");
        return 2;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        lua_pushnil(L);
        lua_pushstring(L, "fork failed");
        return 2;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    auto* slot = static_cast<std::shared_ptr<ProcessHandle>*>(
        lua_newuserdatauv(L, sizeof(std::shared_ptr<ProcessHandle>), 0));
    auto handle = std::make_shared<ProcessHandle>();
    handle->state = state;
    handle->pid = pid;
    handle->cmd = cmd;
    handle->stdin_sd.emplace(state->ioc.get(), stdin_pipe[1]);
    handle->stdout_sd.emplace(state->ioc.get(), stdout_pipe[0]);
    new (slot) std::shared_ptr<ProcessHandle>(std::move(handle));
    luaL_setmetatable(L, kProcessMetatable);
    return 1;
}

}  // namespace

// --- AsyncIOLibrary ---

AsyncIOLibrary::AsyncIOLibrary(asio::io_context& ioc) : ioc_(ioc) {}

AsyncIOLibrary::~AsyncIOLibrary() {
    Close(nullptr);
}

void AsyncIOLibrary::Open(lua_State* L) {
    if (luaL_newmetatable(L, kFileMetatable)) {
        lua_pushcfunction(L, file_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, file_index);
        lua_setfield(L, -2, "__index");
        luaL_newlib(L, file_methods);
        lua_setfield(L, -2, "__methods");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, kProcessMetatable)) {
        lua_pushcfunction(L, process_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, process_index);
        lua_setfield(L, -2, "__index");
        luaL_newlib(L, process_methods);
        lua_setfield(L, -2, "__methods");
    }
    lua_pop(L, 1);

    auto state = std::shared_ptr<AsyncIOState>(
        new AsyncIOState{std::ref(ioc_), false});
    g_async_state.Set(L, std::move(state));

    lua_newtable(L);
    luaL_Reg funcs[] = {
        {"open", async_open},
        {"exec", async_exec},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, funcs, 0);
}

void AsyncIOLibrary::Close(lua_State* L) {
    if (!L) return;
    auto state = g_async_state.Get(L);
    if (!state || state->shutting_down.exchange(true)) return;
    // io_context lifecycle is managed externally — just set the flag
}
