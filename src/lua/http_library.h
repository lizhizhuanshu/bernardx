#pragma once

#include <cinatra/ylt/coro_io/io_context_pool.hpp>

#include <atomic>
#include <memory>

#include "lua_library.h"

struct HttpLibraryState {
    coro_io::ExecutorWrapper<>* exec = nullptr;
    std::atomic<bool> shutting_down{false};
};

class HttpLibrary : public LuaLibrary {
public:
    explicit HttpLibrary(coro_io::ExecutorWrapper<>& exec);
    ~HttpLibrary() override;

    std::string name() const override { return "http"; }
    void Open(lua_State* L) override;
    void Close(lua_State* L) override;

private:
    coro_io::ExecutorWrapper<>& exec_;
};
