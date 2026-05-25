#include "fs_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

// lfs.attributes(path [, attributename])
// Returns a table of file attributes, or a single attribute value.
static int lfs_attributes(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::error_code ec;
    auto st = fs::status(path, ec);
    if (ec) {
        lua_pushnil(L);
        lua_pushstring(L, ec.message().c_str());
        return 2;
    }

    const char* attr_name = lua_tostring(L, 2);
    auto mtime = fs::last_write_time(path, ec);

    if (attr_name) {
        std::string mode;
        if (fs::is_directory(st)) mode = "directory";
        else if (fs::is_regular_file(st)) mode = "file";
        else if (fs::is_symlink(st)) mode = "link";
        else mode = "other";

        if (std::string(attr_name) == "mode") {
            lua_pushstring(L, mode.c_str());
        } else if (std::string(attr_name) == "modification") {
            auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(mtime);
            auto epoch = sctp.time_since_epoch();
            lua_pushinteger(L, static_cast<lua_Integer>(epoch.count()));
        } else if (std::string(attr_name) == "size") {
            auto sz = fs::file_size(path, ec);
            if (ec) {
                lua_pushnil(L);
            } else {
                lua_pushinteger(L, static_cast<lua_Integer>(sz));
            }
        } else {
            lua_pushnil(L);
        }
        return 1;
    }

    lua_newtable(L);
    std::string mode;
    if (fs::is_directory(st)) mode = "directory";
    else if (fs::is_regular_file(st)) mode = "file";
    else if (fs::is_symlink(st)) mode = "link";
    else mode = "other";

    lua_pushstring(L, mode.c_str());
    lua_setfield(L, -2, "mode");

    auto sz = fs::file_size(path, ec);
    if (!ec) {
        lua_pushinteger(L, static_cast<lua_Integer>(sz));
        lua_setfield(L, -2, "size");
    }

    if (!ec || ec == std::errc::is_a_directory) {
        auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(mtime);
        auto epoch = sctp.time_since_epoch();
        lua_pushinteger(L, static_cast<lua_Integer>(epoch.count()));
        lua_setfield(L, -2, "modification");
    }

    return 1;
}

// lfs.currentdir()
static int lfs_currentdir(lua_State* L) {
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) {
        lua_pushnil(L);
        lua_pushstring(L, ec.message().c_str());
        return 2;
    }
    lua_pushstring(L, cwd.string().c_str());
    return 1;
}

// lfs.chdir(path)
static int lfs_chdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::error_code ec;
    fs::current_path(path, ec);
    if (ec) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ec.message().c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// lfs.mkdir(path)
static int lfs_mkdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::error_code ec;
    bool ok = fs::create_directory(path, ec);
    if (ec || !ok) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ec ? ec.message().c_str() : "already exists");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// lfs.rmdir(path)
static int lfs_rmdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ec.message().c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// lfs.dir(path) — returns iterator function (next, state, nil)
static int lfs_dir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::error_code ec;
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return luaL_error(L, "cannot open directory '%s': %s", path, ec.message().c_str());
    }

    // Store the iterator as a full userdata
    using DirIter = fs::directory_iterator;
    auto* ud = static_cast<DirIter*>(lua_newuserdatauv(L, sizeof(DirIter), 0));
    new (ud) DirIter(std::move(it));

    // Metatable with __gc
    luaL_newmetatable(L, "lfs_dir_iter");
    lua_pushcfunction(L, [](lua_State* LS) -> int {
        auto* iter = static_cast<fs::directory_iterator*>(lua_touserdata(LS, 1));
        iter->~directory_iterator();
        return 0;
    });
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    // Push the next function
    lua_pushcclosure(L, [](lua_State* LS) -> int {
        auto* iter = static_cast<fs::directory_iterator*>(lua_touserdata(LS, lua_upvalueindex(1)));
        if (*iter == fs::directory_iterator{}) {
            return 0;
        }
        std::error_code ec;
        auto entry = **iter;
        iter->increment(ec);
        if (ec) {
            return 0;
        }
        lua_pushstring(LS, entry.path().filename().string().c_str());
        return 1;
    }, 1);

    // Also return the state (userdata) as second return, matching lfs convention
    // Actually, lfs.dir returns (next, state, nil) — but for simplicity with generic for:
    // We use closure-captured iterator, so just return next function.
    // Users call: for name in lfs.dir(path) do ... end
    return 1;
}

// lfs.symlinkattributes(path [, attributename])
static int lfs_symlinkattributes(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::error_code ec;
    auto st = fs::symlink_status(path, ec);
    if (ec) {
        lua_pushnil(L);
        lua_pushstring(L, ec.message().c_str());
        return 2;
    }

    const char* attr_name = lua_tostring(L, 2);
    std::string mode;
    if (fs::is_directory(st)) mode = "directory";
    else if (fs::is_regular_file(st)) mode = "file";
    else if (fs::is_symlink(st)) mode = "link";
    else mode = "other";

    if (attr_name) {
        lua_pushstring(L, mode.c_str());
        return 1;
    }

    lua_newtable(L);
    lua_pushstring(L, mode.c_str());
    lua_setfield(L, -2, "mode");
    return 1;
}

// lfs.touch(path [, atime [, mtime]])
static int lfs_touch(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::error_code ec;

    if (!fs::exists(path, ec)) {
        std::ofstream ofs(path);
        if (!ofs) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "cannot create file");
            return 2;
        }
    }

    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(path, now, ec);
    if (ec) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, ec.message().c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg lfs_funcs[] = {
    {"attributes", lfs_attributes},
    {"symlinkattributes", lfs_symlinkattributes},
    {"currentdir", lfs_currentdir},
    {"chdir", lfs_chdir},
    {"mkdir", lfs_mkdir},
    {"rmdir", lfs_rmdir},
    {"dir", lfs_dir},
    {"touch", lfs_touch},
    {nullptr, nullptr}
};

void FileSystemLibrary::Open(lua_State* L) {
    luaL_newlib(L, lfs_funcs);
    lua_setglobal(L, "lfs");
    // Also push as return value for require("lfs")
    lua_getglobal(L, "lfs");
}
