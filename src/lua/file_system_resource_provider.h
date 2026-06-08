#pragma once

#include "resource_provider.h"

#include <string>

class FileSystemResourceProvider : public ResourceProvider {
public:
    explicit FileSystemResourceProvider(std::string resource_dir);
    async_simple::coro::Lazy<std::vector<ResourceEntry>> List(const std::string& relative_path) override;
    async_simple::coro::Lazy<std::optional<std::string>> Load(const std::string& relative_path) override;
private:
    std::string base_dir_;
};
