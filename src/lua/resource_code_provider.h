#pragma once

#include <algorithm>
#include <memory>
#include <string>

#include "code_provider.h"
#include "resource_provider.h"

class ResourceCodeProvider : public CodeProvider {
public:
    ResourceCodeProvider(std::shared_ptr<ResourceProvider> provider,
                         std::vector<std::string> search_paths)
        : provider_(std::move(provider)), search_paths_(std::move(search_paths)) {}

    static std::string ModuleToPath(const std::string& module_name) {
        std::string path = module_name;
        std::replace(path.begin(), path.end(), '.', '/');
        return path;
    }

    async_simple::coro::Lazy<std::optional<std::string>> LoadModule(const std::string& module_name) override {
        auto path = ModuleToPath(module_name);
        if (auto result = co_await TryLoad(path + ".lua")) {
            co_return result;
        }
        co_return co_await TryLoad(path + "/init.lua");
    }

    async_simple::coro::Lazy<std::optional<std::string>> LoadFile(const std::string& path) override {
        co_return co_await TryLoad(path);
    }

private:
    async_simple::coro::Lazy<std::optional<std::string>> TryLoad(const std::string& filename) {
        for (const auto& dir : search_paths_) {
            auto full = dir.empty() ? filename : (dir + "/" + filename);
            auto result = co_await provider_->Load(full);
            if (result.has_value()) {
                co_return result;
            }
        }
        co_return std::nullopt;
    }

    std::shared_ptr<ResourceProvider> provider_;
    std::vector<std::string> search_paths_;
};
