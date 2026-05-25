#include <gflags/gflags.h>

#include <asio.hpp>
#include <async_simple/coro/SyncAwait.h>
#include <async_simple/executors/SimpleExecutor.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "bt_library.h"
#include "http_library.h"
#include "json_library.h"
#include "fs_library.h"
#include "blackboard_library.h"
#include "file_system_code_provider.h"
#include "lua_runtime.h"
#include "blackboard.h"

DEFINE_string(dir, ".", "Working directory containing src/ and libs/");
DEFINE_string(entry, "", "Entry Lua file relative to --dir (default: src/main.lua)");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    std::string dir = FLAGS_dir;
    if (!std::filesystem::is_directory(dir)) {
        std::cerr << "Error: directory not found: " << dir << std::endl;
        return 1;
    }

    asio::io_context ioc{1};
    auto ioc_work = asio::make_work_guard(ioc);
    auto http_exec = std::make_unique<coro_io::ExecutorWrapper<>>(ioc.get_executor());
    std::thread io_thread([&ioc]() { ioc.run(); });

    auto code_provider = std::make_shared<FileSystemCodeProvider>(dir);
    auto blackboard = std::make_shared<Blackboard>();
    auto bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
    auto bt_lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
    bt_lib->SetMainLibsPath(std::filesystem::absolute(dir).string() + "/libs");
    auto http_lib = std::make_shared<HttpLibrary>(*http_exec);
    auto json_lib = std::make_shared<JsonLibrary>();
    auto fs_lib = std::make_shared<FileSystemLibrary>();

    async_simple::executors::SimpleExecutor executor(1);
    auto rt = LuaRuntime::Builder()
                  .WithCodeProvider(code_provider)
                  .WithExecutor(executor)
                  .RegisterLibrary(bb_lib)
                  .RegisterLibrary(bt_lib)
                  .RegisterLibrary(http_lib)
                  .RegisterLibrary(json_lib)
                  .RegisterLibrary(fs_lib)
                  .Create();

    std::string entry = FLAGS_entry.empty() ? "src/main.lua" : FLAGS_entry;
    std::string entry_path = std::filesystem::absolute(dir / std::filesystem::path(entry)).string();
    if (!std::filesystem::exists(entry_path)) {
        std::cerr << "Error: entry file not found: " << entry_path << std::endl;
        return 1;
    }

    auto result = async_simple::coro::syncAwait(rt->RunFile(entry_path));

    if (result.status != 0) {
        std::cerr << entry << " failed: " << result.error << std::endl;
        return 1;
    }

    if (bt_lib->engine() && bt_lib->engine()->IsRunning()) {
        bt_lib->engine()->StopLoop();
        bt_lib->engine()->Stop();
    }

    ioc_work.reset();
    ioc.stop();
    io_thread.join();

    return 0;
}
