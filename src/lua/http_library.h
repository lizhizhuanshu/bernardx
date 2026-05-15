#pragma once

#include <cinatra/ylt/coro_io/io_context_pool.hpp>

#include "lua_library.h"

class HttpLibrary : public LuaLibrary {
public:
    explicit HttpLibrary(int io_threads = 1);
    ~HttpLibrary() override;

    std::string name() const override { return "http"; }
    void Open(lua_State* L) override;
    void Close(lua_State* L) override;

    coro_io::ExecutorWrapper<>* executor() const { return exec_; }

private:
    std::unique_ptr<coro_io::multithread_context_pool> pool_;
    coro_io::ExecutorWrapper<>* exec_ = nullptr;
};
