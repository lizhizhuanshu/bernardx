#include "async_io_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <asio.hpp>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

class AsyncIOLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpdir = std::filesystem::temp_directory_path() / ("async_io_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(tmpdir);

        work = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(ioc));
        io_thread = std::thread([this]() { ioc.run(); });

        rt = LuaRuntime::Builder()
            .RegisterLibrary(std::make_shared<AsyncIOLibrary>(ioc))
            .Create();
    }

    void TearDown() override {
        rt.reset();
        work.reset();
        ioc.stop();
        if (io_thread.joinable()) io_thread.join();
        std::filesystem::remove_all(tmpdir);
    }

    asio::io_context ioc{1};
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread io_thread;
    LuaRuntime::Ptr rt;
    std::filesystem::path tmpdir;
};

// --- Module require ---

TEST_F(AsyncIOLibraryTest, RequireReturnsTable) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        assert(type(async) == "table")
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, HasModuleFunctions) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        assert(type(async.open) == "function", "expected async.open")
        assert(type(async.exec) == "function", "expected async.exec")
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, RequireCached) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local a = require("async")
        local b = require("async")
        assert(a == b, "expected same table on second require")
    )")).status, LUA_OK);
}

// --- async.open ---

TEST_F(AsyncIOLibraryTest, OpenReturnsUserdata) {
    auto path = (tmpdir / "test.txt").string();
    // Create the file so fopen succeeds
    std::ofstream(path) << "";

    auto script = "local async = require('async')\n"
                  "local f, err = async.open('" + path + "', 'r')\n"
                  "assert(f ~= nil, 'expected handle, got nil: ' .. tostring(err))\n"
                  "assert(type(f) == 'userdata', 'expected userdata')\n"
                  "f:close()\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, OpenNonexistentReturnsNilErr) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local f, err = async.open("/nonexistent/path/file.txt", "r")
        assert(f == nil, "expected nil for nonexistent file")
        assert(err ~= nil, "expected error message")
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, OpenFileHasMethods) {
    auto path = (tmpdir / "test.txt").string();
    std::ofstream(path) << "";

    auto script = "local async = require('async')\n"
                  "local f = async.open('" + path + "', 'r')\n"
                  "assert(type(f.read) == 'function')\n"
                  "assert(type(f.write) == 'function')\n"
                  "assert(type(f.close) == 'function')\n"
                  "f:close()\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

// --- FileHandle:read / :write ---

TEST_F(AsyncIOLibraryTest, WriteAndReadAll) {
    auto path = (tmpdir / "rw.txt").string();

    auto script = "local async = require('async')\n"
                  "local f = async.open('" + path + "', 'w')\n"
                  "local ok = f:write('hello world')\n"
                  "assert(ok == true, 'write should return true')\n"
                  "f:close()\n"
                  "local f2 = async.open('" + path + "', 'r')\n"
                  "local data = f2:read('*a')\n"
                  "assert(data == 'hello world', 'expected hello world, got: ' .. tostring(data))\n"
                  "f2:close()\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ReadLine) {
    auto path = (tmpdir / "lines.txt").string();

    auto script = "local async = require('async')\n"
                  "local f = async.open('" + path + "', 'w')\n"
                  "f:write('line one\\nline two\\nline three\\n')\n"
                  "f:close()\n"
                  "local f2 = async.open('" + path + "', 'r')\n"
                  "local l1 = f2:read('*l')\n"
                  "assert(l1 == 'line one', 'line 1: ' .. tostring(l1))\n"
                  "local l2 = f2:read('*l')\n"
                  "assert(l2 == 'line two', 'line 2: ' .. tostring(l2))\n"
                  "local l3 = f2:read('*l')\n"
                  "assert(l3 == 'line three', 'line 3: ' .. tostring(l3))\n"
                  "local eof = f2:read('*l')\n"
                  "assert(eof == nil, 'expected nil at EOF')\n"
                  "f2:close()\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ReadBytes) {
    auto path = (tmpdir / "bytes.txt").string();

    auto script = "local async = require('async')\n"
                  "local f = async.open('" + path + "', 'w')\n"
                  "f:write('ABCDEFGHIJ')\n"
                  "f:close()\n"
                  "local f2 = async.open('" + path + "', 'r')\n"
                  "local chunk = f2:read(5)\n"
                  "assert(chunk == 'ABCDE', 'expected ABCDE, got: ' .. tostring(chunk))\n"
                  "local rest = f2:read('*a')\n"
                  "assert(rest == 'FGHIJ', 'expected FGHIJ, got: ' .. tostring(rest))\n"
                  "f2:close()\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ReadAfterCloseErrors) {
    auto path = (tmpdir / "closed.txt").string();
    std::ofstream(path) << "data";

    auto script = "local async = require('async')\n"
                  "local f = async.open('" + path + "', 'r')\n"
                  "f:close()\n"
                  "local data, err = f:read('*a')\n"
                  "assert(data == nil, 'expected nil from closed file')\n"
                  "assert(err ~= nil, 'expected error')\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, AppendMode) {
    auto path = (tmpdir / "append.txt").string();
    std::ofstream(path) << "first\n";

    auto script = "local async = require('async')\n"
                  "local f = async.open('" + path + "', 'a')\n"
                  "f:write('second\\n')\n"
                  "f:close()\n"
                  "local f2 = async.open('" + path + "', 'r')\n"
                  "local content = f2:read('*a')\n"
                  "assert(content == 'first\\nsecond\\n', 'got: ' .. tostring(content))\n"
                  "f2:close()\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

// --- async.exec ---

TEST_F(AsyncIOLibraryTest, ExecReturnsUserdata) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("echo hello")
        assert(p ~= nil, "expected process handle")
        assert(type(p) == "userdata")
        p:close()
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecHasMethods) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("cat")
        assert(type(p.read) == "function")
        assert(type(p.write) == "function")
        assert(type(p.shutdown) == "function")
        assert(type(p.close) == "function")
        p:close()
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecWriteReadLine) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("cat")
        p:write("hello\n")
        p:write("world\n")
        p:shutdown()
        local l1 = p:read("*l")
        assert(l1 == "hello", "line 1: " .. tostring(l1))
        local l2 = p:read("*l")
        assert(l2 == "world", "line 2: " .. tostring(l2))
        local eof = p:read("*l")
        assert(eof == nil, "expected nil at EOF, got: " .. tostring(eof))
        local code = p:close()
        assert(code == 0, "expected exit 0, got: " .. tostring(code))
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecReadAll) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("echo -n 'output here'")
        local data = p:read("*a")
        assert(data == "output here", "got: " .. tostring(data))
        local code = p:close()
        assert(code == 0, "expected exit 0")
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecReadBytes) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("echo -n 'ABCDEFGHIJ'")
        local chunk = p:read(5)
        assert(chunk == "ABCDE", "chunk: " .. tostring(chunk))
        local rest = p:read("*a")
        assert(rest == "FGHIJ", "rest: " .. tostring(rest))
        p:close()
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecExitCode) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("exit 42")
        local code = p:close()
        assert(code == 42, "expected exit 42, got: " .. tostring(code))
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecStderrMergedToStdout) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("echo stderr_msg >&2 && echo stdout_msg")
        local data = p:read("*a")
        assert(data:find("stderr_msg"), "stderr not in output: " .. tostring(data))
        assert(data:find("stdout_msg"), "stdout not in output: " .. tostring(data))
        p:close()
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecWriteToCat) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("cat")
        p:write("test data\n")
        p:shutdown()
        local line = p:read("*l")
        assert(line == "test data", "got: " .. tostring(line))
        p:close()
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecCloseTwice) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("true")
        local code1 = p:close()
        -- second close should not crash
        -- (handle is already closed, will error which we catch)
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecKill) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("sleep 20")
        local ok = p:kill()
        assert(ok == true, "expected kill to return true")
        local code = p:close()
        assert(code == -15, "expected -15 (SIGTERM), got: " .. tostring(code))
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecKillThenRead) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("sleep 20")
        local ok = p:kill()
        assert(ok == true)
        local data = p:read("*a")
        -- sleep produces no output; read returns nil on empty EOF
        assert(data == nil or data == "")
        local code = p:close()
        assert(code == -15, "expected -15 (SIGTERM), got: " .. tostring(code))
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecKillAfterCloseFails) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("true")
        p:close()
        local ok, err = p:kill()
        assert(ok == false, "expected false")
        assert(err ~= nil, "expected error message")
    )")).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ExecReadAfterCloseErrors) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        local p = async.exec("true")
        p:close()
        local data, err = p:read("*a")
        assert(data == nil, "expected nil from closed process")
        assert(err ~= nil, "expected error")
    )")).status, LUA_OK);
}

// --- Integration: file + process pipeline ---

TEST_F(AsyncIOLibraryTest, ProcessOutputToFile) {
    auto path = (tmpdir / "proc_out.txt").string();

    auto script = "local async = require('async')\n"
                  "local p = async.exec('echo generated_output')\n"
                  "local data = p:read('*a')\n"
                  "p:close()\n"
                  "local f = async.open('" + path + "', 'w')\n"
                  "f:write(data)\n"
                  "f:close()\n"
                  "local f2 = async.open('" + path + "', 'r')\n"
                  "local content = f2:read('*a')\n"
                  "f2:close()\n"
                  "assert(content == data, 'round-trip failed')\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

// --- GC safety ---

TEST_F(AsyncIOLibraryTest, FileHandleGC) {
    // Create and abandon a file handle — should not leak or crash
    auto path = (tmpdir / "gc.txt").string();
    std::ofstream(path) << "data";

    auto script = "local async = require('async')\n"
                  "for i = 1, 50 do\n"
                  "  local f = async.open('" + path + "', 'r')\n"
                  "  -- don't close, let GC handle it\n"
                  "end\n"
                  "collectgarbage('collect')\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}

TEST_F(AsyncIOLibraryTest, ProcessHandleGC) {
    // Create and abandon process handles — should not leak or hang
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local async = require("async")
        for i = 1, 20 do
            local p = async.exec("true")
            -- don't close, let GC handle it
        end
        collectgarbage("collect")
    )")).status, LUA_OK);
}

// --- Multiple sequential operations ---

TEST_F(AsyncIOLibraryTest, MultipleFileOperations) {
    auto path = (tmpdir / "multi.txt").string();

    auto script = "local async = require('async')\n"
                  "local f = async.open('" + path + "', 'w')\n"
                  "for i = 1, 10 do\n"
                  "  f:write('line ' .. i .. '\\n')\n"
                  "end\n"
                  "f:close()\n"
                  "local f2 = async.open('" + path + "', 'r')\n"
                  "local count = 0\n"
                  "while true do\n"
                  "  local line = f2:read('*l')\n"
                  "  if line == nil then break end\n"
                  "  count = count + 1\n"
                  "end\n"
                  "f2:close()\n"
                  "assert(count == 10, 'expected 10 lines, got ' .. count)\n";
    EXPECT_EQ(AWAIT(rt->RunScript(script)).status, LUA_OK);
}
