#include "fs_library.h"
#include "lua_runtime.h"

#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

#define AWAIT(lazy) async_simple::coro::syncAwait(lazy)

namespace fs = std::filesystem;

class FsLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        rt = LuaRuntime::Builder()
            .RegisterLibrary(std::make_shared<FileSystemLibrary>())
            .Create();
        tmp_ = fs::temp_directory_path() /
               ("bernardx_lfs_test_" + std::to_string(::getpid()));
        fs::create_directories(tmp_, ec_);
        ASSERT_FALSE(ec_) << ec_.message();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_, ec);
    }

    LuaRuntime::Ptr rt;
    fs::path tmp_;
    std::error_code ec_;
};

TEST_F(FsLibraryTest, RequireReturnsTable) {
    EXPECT_EQ(AWAIT(rt->RunScript(R"(
        local lfs = require("lfs")
        assert(type(lfs) == "table")
        assert(type(lfs.dir) == "function")
        assert(type(lfs.rmtree) == "function")
    )")).status, LUA_OK);
}

TEST_F(FsLibraryTest, DirIteratesEntries) {
    fs::create_directories(tmp_ / "sub", ec_);
    std::ofstream(tmp_ / "a.txt") << "x";

    auto r = AWAIT(rt->RunScript(
        "local lfs = require('lfs') "
        "local names = {} for n in lfs.dir('" + tmp_.string() + "') do names[#names+1] = n end "
        "table.sort(names) "
        "return #names, table.concat(names, ',')"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    // "." and ".." are not yielded by std::filesystem; expect exactly our two entries
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 2);
    EXPECT_EQ(std::get<std::string>(r.values[1]), "a.txt,sub");
}

TEST_F(FsLibraryTest, DirOnMissingPathDoesNotRaise) {
    const std::string missing = (tmp_ / "no_such_dir").string();
    auto r = AWAIT(rt->RunScript(
        "local lfs = require('lfs') "
        "local n = 0 for _ in lfs.dir('" + missing + "') do n = n + 1 end return n"));
    ASSERT_EQ(r.status, LUA_OK) << r.error;
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 0);
}

TEST_F(FsLibraryTest, DirOnMissingPathReturnsErrorAsSecondValue) {
    const std::string missing = (tmp_ / "no_such_dir").string();
    auto r = AWAIT(rt->RunScript(
        "local lfs = require('lfs') "
        "local iter, err = lfs.dir('" + missing + "') "
        "assert(type(iter) == 'function') "
        "assert(err and err:find('cannot open', 1, true)) "
        "assert(iter() == nil) "
        "return err"));
    ASSERT_EQ(r.status, LUA_OK) << r.error;
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_NE(std::get<std::string>(r.values[0]).find("no_such_dir"), std::string::npos);
}
