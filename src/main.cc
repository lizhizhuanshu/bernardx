#include <gflags/gflags.h>

#include <async_simple/coro/SyncAwait.h>
#include <async_simple/executors/SimpleExecutor.h>

#include <filesystem>
#include <fstream>
#include <iostream>

#include "bt_library.h"
#include "http_library.h"
#include "code_provider.h"
#include "lua_runtime.h"

DEFINE_string(dir, ".", "Working directory containing main.lua");

// FileSystemCodeProvider: loads Lua modules from src/ and libs/ subdirectories.
// LoadModule("foo") → tries base/src/foo.lua then base/libs/foo.lua
// LoadFile("bar.lua") → tries base/src/bar.lua then base/libs/bar.lua
class FileSystemCodeProvider : public CodeProvider {
public:
    explicit FileSystemCodeProvider(const std::string& base_dir)
        : base_dir_(std::filesystem::absolute(base_dir).string()) {
        search_paths_ = {
            base_dir_ + "/src",
            base_dir_ + "/libs",
        };
    }

    async_simple::coro::Lazy<std::optional<std::string>> LoadModule(const std::string& module_name) override {
        co_return TryLoad(module_name + ".lua");
    }

    async_simple::coro::Lazy<std::optional<std::string>> LoadFile(const std::string& path) override {
        co_return TryLoad(path);
    }

private:
    std::optional<std::string> TryLoad(const std::string& filename) const {
        for (const auto& dir : search_paths_) {
            auto full = dir + "/" + filename;
            std::ifstream ifs(full, std::ios::in | std::ios::binary);
            if (ifs.is_open()) {
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
                return content;
            }
        }
        return std::nullopt;
    }

    std::string base_dir_;
    std::vector<std::string> search_paths_;
};

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    std::string dir = FLAGS_dir;
    if (!std::filesystem::is_directory(dir)) {
        std::cerr << "Error: directory not found: " << dir << std::endl;
        return 1;
    }

    auto code_provider = std::make_shared<FileSystemCodeProvider>(dir);
    auto bt_lib = std::make_shared<BehaviorTreeLibrary>();
    auto http_lib = std::make_shared<HttpLibrary>();

    async_simple::executors::SimpleExecutor executor(1);
    auto rt = LuaRuntime::Builder()
                  .WithCodeProvider(code_provider)
                  .WithExecutor(executor)
                  .RegisterLibrary(bt_lib)
                  .RegisterLibrary(http_lib)
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
