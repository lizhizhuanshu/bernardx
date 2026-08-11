#include "async_io_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "library_state.h"
#include "lua_runtime.h"

#include <asio.hpp>

// --- Platform abstraction ---------------------------------------------------
// POSIX uses asio::posix::stream_descriptor over an int fd; Windows uses
// asio::windows::stream_handle over a HANDLE opened for overlapped IO. Both
// expose the same async interface (async_read_some / async_write / close), so
// the Lua-level logic (line reader, metatables, GC) is shared below.
#ifdef _WIN32
  #include <asio/windows/stream_handle.hpp>

  #include <windows.h>

  #include <system_error>
  using AsyncStream = asio::windows::stream_handle;
#else
  #include <asio/posix/stream_descriptor.hpp>

  #include <cerrno>
  #include <cstring>
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
  using AsyncStream = asio::posix::stream_descriptor;
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// --- State management ---

namespace {

constexpr const char* kFileMetatable = "async__file";
constexpr const char* kProcessMetatable = "async__process";

LibraryState<AsyncIOState> g_async_state{"async__state"};

// --- OS helpers -------------------------------------------------------------

#ifdef _WIN32

// Last error string from a Win32 error code (default: GetLastError()).
static std::string os_last_error(DWORD e = GetLastError()) {
  char buf[256] = {};
  if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     buf, sizeof(buf), nullptr)) {
    // Strip trailing whitespace/newlines FormatMessage appends.
    size_t n = std::strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '.')) {
      buf[--n] = '\0';
    }
    return std::string(buf);
  }
  return "error " + std::to_string(e);
}

// A one-directional overlapped pipe. The parent keeps the overlapped server
// end; the child inherits the non-overlapped client end. Named pipes are used
// (instead of anonymous CreatePipe) because only they can carry
// FILE_FLAG_OVERLAPPED, which asio::windows::stream_handle requires.
struct OsPipe {
  HANDLE parent = nullptr;  // overlapped, owned by parent (asio wraps it)
  HANDLE child = nullptr;   // non-overlapped, inheritable, passed to child
};

static unsigned long g_pipe_seq = 0;  // monotonic name suffix (single-threaded call sites)

static bool os_make_pipe(OsPipe& out, bool parent_reads, std::string& err) {
  char name[128];
  std::snprintf(name, sizeof(name), "\\\\.\\pipe\\asyncio-%lu-%lu",
                GetCurrentProcessId(), g_pipe_seq++);

  SECURITY_ATTRIBUTES sa_no_inherit{sizeof(sa_no_inherit), nullptr, FALSE};
  SECURITY_ATTRIBUTES sa_inherit{sizeof(sa_inherit), nullptr, TRUE};

  DWORD parent_access = parent_reads
                            ? (PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED)
                            : (PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED);

  // Server end: overlapped, NOT inherited (so closing it in the parent signals
  // EOF — the child is the only other end-holder via the inherited client).
  HANDLE server = CreateNamedPipeA(
      name, parent_access, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1, 4096, 4096, 0, &sa_no_inherit);
  if (server == INVALID_HANDLE_VALUE) {
    err = "CreateNamedPipe: " + os_last_error();
    return false;
  }

  DWORD client_access = parent_reads ? GENERIC_WRITE : GENERIC_READ;
  // Client end: inheritable, non-overlapped, opened against the same instance.
  HANDLE client = CreateFileA(name, client_access, 0, &sa_inherit,
                              OPEN_EXISTING, 0, nullptr);
  if (client == INVALID_HANDLE_VALUE) {
    err = "pipe client: " + os_last_error();
    CloseHandle(server);
    return false;
  }
  // The instance is now connected (client opened it); no ConnectNamedPipe needed.
  out.parent = server;
  out.child = client;
  return true;
}

#else  // POSIX

static std::string os_last_error(int e = errno) {
  return std::string(std::strerror(e));
}

#endif

// --- Handle types ---

struct FileHandle {
  std::shared_ptr<AsyncIOState> state;
  std::optional<AsyncStream> sd;
  std::atomic<bool> closed{false};
  std::string path;
  std::string read_buffer;
};

struct ProcessHandle {
  std::shared_ptr<AsyncIOState> state;
#ifdef _WIN32
  HANDLE proc = nullptr;  // process handle from CreateProcess
#else
  pid_t pid = -1;
#endif
  std::optional<AsyncStream> stdin_sd;
  std::optional<AsyncStream> stdout_sd;
  std::atomic<bool> closed{false};
  std::string cmd;
  std::string read_buffer;
};

// --- Handle accessors ---

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
                  AsyncStream& sd,
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
#ifdef _WIN32
            if (h->proc) {
                TerminateProcess(h->proc, 1);
                CloseHandle(h->proc);
                h->proc = nullptr;
            }
#else
            if (h->pid > 0) {
                kill(h->pid, SIGTERM);
                waitpid(h->pid, nullptr, WNOHANG);
                h->pid = -1;
            }
#endif
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
#ifdef _WIN32
    // Windows has no signals; the signal argument is ignored. TerminateProcess
    // requests termination with the given exit code (1 mirrors a generic kill).
    (void)luaL_optinteger(L, 2, 0);

    if (handle->closed.load()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "process is closed");
        return 2;
    }
    if (!handle->proc) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "no process to kill");
        return 2;
    }
    if (!TerminateProcess(handle->proc, 1)) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "kill failed: %s", os_last_error().c_str());
        return 2;
    }
#else
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
        lua_pushfstring(L, "kill failed: %s", os_last_error().c_str());
        return 2;
    }
#endif
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

#ifdef _WIN32
    if (!handle->proc) {
#else
    if (handle->pid <= 0) {
#endif
        lua_pushinteger(L, 0);
        return 1;
    }

    auto rt = LuaRuntime::FromLuaState(L);
    auto yield_handle = rt->PreYield(L);
    auto shared_h = process_get(L, 1);

    asio::post(shared_h->state->ioc.get(),
        [rt, yield_handle, shared_h]() mutable {
#ifdef _WIN32
            // Blocking wait mirrors the POSIX waitpid-on-ioc behaviour. The
            // process exits (or was already killed); reap its exit code.
            HANDLE proc = shared_h->proc;
            shared_h->proc = nullptr;
            if (!proc) {
                rt->PushResume(yield_handle, {LuaValue{(int64_t)0}});
                return;
            }
            WaitForSingleObject(proc, INFINITE);
            DWORD code = 0;
            int64_t exit_code = -1;
            if (GetExitCodeProcess(proc, &code)) {
                exit_code = static_cast<int64_t>(static_cast<int32_t>(code));
            }
            CloseHandle(proc);
            rt->PushResume(yield_handle, {LuaValue{exit_code}});
#else
            pid_t pid = shared_h->pid;
            shared_h->pid = -1;

            int status = 0;
            int rc = waitpid(pid, &status, 0);

            if (rc < 0) {
                rt->PushResume(yield_handle, {
                    LuaValue{(int64_t)-1},
                    LuaValue{std::string("waitpid failed: ") + os_last_error()}
                });
                return;
            }

            int exit_code = -1;
            if (WIFEXITED(status))
                exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                exit_code = -WTERMSIG(status);
            rt->PushResume(yield_handle, {LuaValue{(int64_t)exit_code}});
#endif
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

    // Open the handle in overlapped mode (Windows) / as a raw fd (POSIX).
#ifdef _WIN32
    DWORD access = GENERIC_READ;
    DWORD disposition = OPEN_EXISTING;
    if (strcmp(mode, "r") == 0) { access = GENERIC_READ; disposition = OPEN_EXISTING; }
    else if (strcmp(mode, "w") == 0) { access = GENERIC_WRITE; disposition = CREATE_ALWAYS; }
    else if (strcmp(mode, "a") == 0) { access = FILE_APPEND_DATA; disposition = OPEN_ALWAYS; }
    else if (strcmp(mode, "r+") == 0) { access = GENERIC_READ | GENERIC_WRITE; disposition = OPEN_EXISTING; }
    else if (strcmp(mode, "w+") == 0) { access = GENERIC_READ | GENERIC_WRITE; disposition = CREATE_ALWAYS; }
    else { return luaL_error(L, "invalid mode '%s'", mode); }

    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           disposition, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open file '%s': %s", path, os_last_error().c_str());
        return 2;
    }

    auto* slot = static_cast<std::shared_ptr<FileHandle>*>(
        lua_newuserdatauv(L, sizeof(std::shared_ptr<FileHandle>), 0));
    auto handle = std::make_shared<FileHandle>();
    handle->state = state;
    handle->path = path;
    handle->sd.emplace(state->ioc.get(), h);
    new (slot) std::shared_ptr<FileHandle>(std::move(handle));
    luaL_setmetatable(L, kFileMetatable);
    return 1;
#else
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
        lua_pushfstring(L, "cannot open file '%s': %s", path, os_last_error().c_str());
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
#endif
}

static int async_exec(lua_State* L) {
    const char* cmd = luaL_checkstring(L, 1);

    auto state = g_async_state.Get(L);
    if (!state || state->shutting_down.load()) {
        return luaL_error(L, "async library is shutting down");
    }

    // Build a process with redirected stdin/stdout (stderr merged into stdout).
    // Parent keeps overlapped handles for async IO; child inherits plain ones.
#ifdef _WIN32
    OsPipe stdin_pipe;    // parent writes -> child stdin
    OsPipe stdout_pipe;   // child stdout/stderr -> parent reads
    std::string err;
    if (!os_make_pipe(stdin_pipe, /*parent_reads=*/false, err) ||
        !os_make_pipe(stdout_pipe, /*parent_reads=*/true, err)) {
        if (stdin_pipe.parent) CloseHandle(stdin_pipe.parent);
        if (stdin_pipe.child) CloseHandle(stdin_pipe.child);
        if (stdout_pipe.parent) CloseHandle(stdout_pipe.parent);
        if (stdout_pipe.child) CloseHandle(stdout_pipe.child);
        lua_pushnil(L);
        lua_pushfstring(L, "pipe creation failed: %s", err.c_str());
        return 2;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_pipe.child;
    si.hStdOutput = stdout_pipe.child;
    si.hStdError = stdout_pipe.child;  // merge stderr into stdout

    // Execute via cmd.exe /c, mirroring POSIX "/bin/sh -c".
    std::string cmdline = std::string("cmd.exe /c ") + cmd;
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');  // CreateProcess may mutate; needs writable buffer

    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr,
                        /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &si, &pi)) {
        std::string e = os_last_error();
        CloseHandle(stdin_pipe.parent); CloseHandle(stdin_pipe.child);
        CloseHandle(stdout_pipe.parent); CloseHandle(stdout_pipe.child);
        lua_pushnil(L);
        lua_pushfstring(L, "CreateProcess failed: %s", e.c_str());
        return 2;
    }

    // Parent no longer needs the child ends — closing them lets reads see EOF
    // once the child exits / closes its copies.
    CloseHandle(stdin_pipe.child);
    CloseHandle(stdout_pipe.child);
    CloseHandle(pi.hThread);

    auto* slot = static_cast<std::shared_ptr<ProcessHandle>*>(
        lua_newuserdatauv(L, sizeof(std::shared_ptr<ProcessHandle>), 0));
    auto handle = std::make_shared<ProcessHandle>();
    handle->state = state;
    handle->proc = pi.hProcess;
    handle->cmd = cmd;
    handle->stdin_sd.emplace(state->ioc.get(), stdin_pipe.parent);
    handle->stdout_sd.emplace(state->ioc.get(), stdout_pipe.parent);
    new (slot) std::shared_ptr<ProcessHandle>(std::move(handle));
    luaL_setmetatable(L, kProcessMetatable);
    return 1;
#else
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
#endif
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
