#pragma once

#include <asio.hpp>

#include <atomic>
#include <functional>
#include <memory>

#include "lua_library.h"

struct AsyncIOState {
    std::reference_wrapper<asio::io_context> ioc;
    std::atomic<bool> shutting_down{false};
};

class AsyncIOLibrary : public LuaLibrary {
public:
    explicit AsyncIOLibrary(asio::io_context& ioc);
    ~AsyncIOLibrary() override;

    std::string name() const override { return "async"; }
    void Open(lua_State* L) override;
    void Close(lua_State* L) override;

private:
    asio::io_context& ioc_;
};
