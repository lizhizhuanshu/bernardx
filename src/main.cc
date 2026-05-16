#include <gflags/gflags.h>

#include <async_simple/coro/SyncAwait.h>
#include <async_simple/executors/SimpleExecutor.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "bt_library.h"
#include "http_library.h"
#include "json_library.h"
#include "file_system_code_provider.h"
#include "lua_runtime.h"

DEFINE_string(dir, ".", "Working directory containing main.lua");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    std::string dir = FLAGS_dir;
    if (!std::filesystem::is_directory(dir)) {
        std::cerr << "Error: directory not found: " << dir << std::endl;
        return 1;
    }

    auto code_provider = std::make_shared<FileSystemCodeProvider>(dir);
    auto bt_lib = std::make_shared<BehaviorTreeLibrary>();
    bt_lib->SetMainLibsPath(std::filesystem::absolute(dir).string() + "/libs");
    auto http_lib = std::make_shared<HttpLibrary>();
    auto json_lib = std::make_shared<JsonLibrary>();

    async_simple::executors::SimpleExecutor executor(1);
    auto rt = LuaRuntime::Builder()
                  .WithCodeProvider(code_provider)
                  .WithExecutor(executor)
                  .RegisterLibrary(bt_lib)
                  .RegisterLibrary(http_lib)
                  .RegisterLibrary(json_lib)
                  .Create();

    std::string main_lua = std::filesystem::absolute(dir).string() + "/src/main.lua";
    auto result = async_simple::coro::syncAwait(rt->RunFile(main_lua));

    if (result.status != 0) {
        std::cerr << "main.lua failed: " << result.error << std::endl;
        return 1;
    }

    if (bt_lib->engine() && bt_lib->engine()->IsRunning()) {
        bt_lib->StopBtThread(true);
        bt_lib->engine()->Stop();
    }

    return 0;
}
