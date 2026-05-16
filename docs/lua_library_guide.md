# Lua Library 开发规范

本文档描述如何在 bernardx 项目中实现新的 Lua Library。

## 架构概览

```
LuaLibrary (基类)
├── JsonLibrary    — 纯同步库（json.encode / json.decode）
├── HttpLibrary    — 异步 I/O 库（HTTP 请求 + WebSocket）
└── BehaviorTreeLibrary — 异步库（行为树执行，upvalue 注入上下文）
```

所有库通过 `LuaRuntime::Builder::RegisterLibrary()` 注册，Lua 脚本通过 `require('name')` 加载。

---

## 1. 基类接口

```cpp
// src/lua/lua_library.h
class LuaLibrary {
public:
    virtual ~LuaLibrary() = default;
    virtual std::string name() const = 0;   // 模块名，用于 require('name')
    virtual void Open(lua_State* L) = 0;    // 模块加载时调用，栈顶应留下一个 table
    virtual void Close(lua_State* L) {}     // 可选：运行时关闭时清理资源
};
```

**规则：**
- `name()` 返回小写字符串，如 `"json"`、`"http"`、`"bt"`
- `Open()` 执行完毕后，Lua 栈顶必须是该模块的 table
- `Close()` 用于释放异步资源、设置 shutdown 标志等

---

## 2. 注册与加载流程

### C++ 侧注册

```cpp
auto rt = LuaRuntime::Builder()
    .RegisterLibrary(std::make_shared<MyLibrary>())
    .Create();
```

### Lua 侧加载

```lua
local my = require('my')  -- 调用 MyLibrary::Open()
```

### require 查找顺序

1. `package.loaded` 缓存 → 命中则直接返回
2. C 模块（`Builder::Register`）→ 同步调用 `lua_CFunction`
3. **LuaLibrary**（`Builder::RegisterLibrary`）→ 同步调用 `Open()`
4. CodeProvider → 异步加载 Lua 源码

---

## 3. Open() 实现模式

### 3.1 纯同步库（无状态）

```cpp
void MyLibrary::Open(lua_State* L) {
    lua_newtable(L);                            // 创建模块 table
    luaL_Reg funcs[] = {
        {"foo", my_foo},
        {"bar", my_bar},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, funcs, 0);                 // 注册函数，0 个 upvalue
    // 栈顶 = 模块 table，require 返回此 table
}
```

**适用场景：** 纯计算、数据转换（如 JsonLibrary）

### 3.2 带状态的库

```cpp
void MyLibrary::Open(lua_State* L) {
    // 1. 创建 metatable（幂等）
    if (luaL_newmetatable(L, "my__state")) {
        lua_pushcfunction(L, my_state_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    // 2. 创建状态并存入 registry
    auto state = std::make_shared<MyState>();
    SetMyState(L, state);  // userdata + registry 存储

    // 3. 创建模块 table + 注册函数
    lua_newtable(L);
    luaL_Reg funcs[] = {
        {"do_something", my_do_something},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, funcs, 0);
}
```

**状态存储方式：**
- 使用 `lua_newuserdatauv` 分配 `shared_ptr<MyState>` 大小的 userdata
- 设置带 `__gc` 的 metatable 用于析构
- 通过 `lua_rawsetp(L, LUA_REGISTRYINDEX, &key)` 存入 registry

**适用场景：** HTTP 连接池、异步执行器、外部资源管理

### 3.3 带 Upvalue 的库

```cpp
void MyLibrary::Open(lua_State* L) {
    lua_pushlightuserdata(L, engine_.get());  // upvalue 1
    lua_pushlightuserdata(L, this);            // upvalue 2
    luaL_Reg funcs[] = {
        {"run",  bt_run},
        {"stop", bt_stop},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, funcs, 2);  // 2 个 upvalue

    lua_newtable(L);
    // 栈: upvalue1, upvalue2, table <- 栈顶
    // 注意：带 upvalue 时需调整栈操作顺序
}
```

**Upvalue 获取：**
```cpp
static MyEngine* GetEngine(lua_State* L) {
    return static_cast<MyEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
}
```

**适用场景：** 需要在 C 函数中访问 C++ 对象（引擎、库实例等）

---

## 4. 异步操作模式

异步操作使用 **yield/resume** 模式与 Lua 协程配合。

### 4.1 基本异步模式

```cpp
static int my_async_op(lua_State* L) {
    auto rt = LuaContext::FromLuaState(L);
    auto handle = rt->PreYield(L);           // 1. 注册挂起条目

    // 2. 启动异步操作
    StartAsyncWork([rt, handle](Result result) {
        // 3. 异步完成，推入结果并恢复协程
        rt->PushResume(handle, {
            LuaValue{result.status},
            LuaValue{result.data}
        });
    });

    return rt->Yield(L);                     // 4. 挂起协程
}
```

**关键步骤：**
1. `PreYield(L)` — 注册当前协程为 pending，返回 handle
2. 在异步回调中调用 `PushResume(handle, values)` — 传入返回值
3. `Yield(L)` — 执行 `lua_yield` 挂起协程

**Lua 侧表现为同步调用：**
```lua
local status, data = my.async_op()  -- 自动 yield/resume
```

### 4.2 异步模板（HttpLibrary 风格）

```cpp
template <typename F>
static int my_request(lua_State* L, F&& make_lazy) {
    auto state = GetMyState(L);
    if (!state || state->shutting_down.load()) {
        return luaL_error(L, "library is shutting down");
    }

    auto rt = LuaContext::FromLuaState(L);
    auto handle = rt->PreYield(L);

    asio::post(state->exec->context(), [
        state, rt, handle,
        factory = std::forward<F>(make_lazy)
    ]() mutable {
        auto lazy = factory();
        std::move(lazy).via(state->exec).start([
            rt, handle
        ](auto&& result) {
            // 处理结果并 PushResume
            rt->PushResume(handle, BuildResultValues(result));
        });
    });

    return rt->Yield(L);
}
```

### 4.3 PushResume 返回值构造

```cpp
// 成功
rt->PushResume(handle, {
    LuaValue{true},                    // ok
    LuaValue{std::string("data")},     // 结果
    LuaValue{nullptr}                  // 无错误
});

// 失败
rt->PushResume(handle, {
    LuaValue{false},                   // ok
    LuaValue{nullptr},                 // 无结果
    LuaValue{std::string("error msg")} // 错误信息
});
```

---

## 5. 参数解析约定

### 基本类型

```cpp
// 必选参数
const char* url = luaL_checkstring(L, 1);
int64_t ms = luaL_checkinteger(L, 2);

// 可选参数
std::string body = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
const char* ct = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr;

// 带长度的字符串
size_t len;
const char* str = luaL_checklstring(L, 1, &len);
std::string data(str, len);
```

### Table 参数（headers 等）

```cpp
static std::unordered_map<std::string, std::string> parse_table(lua_State* L, int idx) {
    std::unordered_map<std::string, std::string> result;
    if (lua_isnil(L, idx) || !lua_istable(L, idx)) {
        return result;
    }
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
            result.emplace(lua_tostring(L, -2), lua_tostring(L, -1));
        }
        lua_pop(L, 1);  // pop value, keep key
    }
    return result;
}
```

---

## 6. Metatable 与 Userdata 模式

用于面向对象风格的 Lua 对象（如 WebSocket 连接）。

### 6.1 创建 Metatable

```cpp
// Open() 中注册，幂等（luaL_newmetatable 在不存在时创建并返回 true）
if (luaL_newmetatable(L, "my__obj")) {
    lua_pushcfunction(L, obj_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, obj_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, obj_newindex);
    lua_setfield(L, -2, "__newindex");

    // 方法表（供 __index 回退查找）
    luaL_newlib(L, obj_methods);
    lua_setfield(L, -2, "__methods");
}
lua_pop(L, 1);
```

### 6.2 创建 Userdata

```cpp
// 在栈上分配 shared_ptr<MyObj> 大小的 userdata
auto* slot = static_cast<std::shared_ptr<MyObj>*>(
    lua_newuserdatauv(L, sizeof(std::shared_ptr<MyObj>), 0));
new (slot) std::shared_ptr<MyObj>(std::make_shared<MyObj>());
luaL_setmetatable(L, "my__obj");
```

### 6.3 GC 函数

```cpp
static int obj_gc(lua_State* L) {
    auto* slot = static_cast<std::shared_ptr<MyObj>*>(
        luaL_testudata(L, 1, "my__obj"));
    if (slot) {
        // 清理资源
        (*slot)->close();
        slot->~shared_ptr();  // 手动析构
    }
    return 0;
}
```

### 6.4 __index / __newindex

```cpp
// 属性读取
static int obj_index(lua_State* L) {
    auto* obj = check_obj(L, 1);
    const char* key = lua_tostring(L, 2);

    if (key && strcmp(key, "status") == 0) {
        lua_pushstring(L, obj->status().c_str());
        return 1;
    }

    // 回退到方法表
    luaL_getmetatable(L, "my__obj");
    lua_getfield(L, -1, "__methods");
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    return 1;
}

// 属性写入（回调函数等）
static int obj_newindex(lua_State* L) {
    auto* obj = check_obj(L, 1);
    const char* key = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    // 保存 ref 到对象中...
    return 0;
}
```

---

## 7. 命名约定

| 类别 | 规则 | 示例 |
|------|------|------|
| 类名 | PascalCase + Library 后缀 | `JsonLibrary`, `HttpLibrary` |
| 模块名 | 小写单词 | `"json"`, `"http"`, `"bt"` |
| C 函数 | `模块_方法` snake_case | `json_decode`, `http_get`, `bt_run` |
| Metatable | `模块__类型` | `"http__ws"`, `"http__state"` |
| Registry Key | 文件作用域静态变量地址 | `static int kMyKey = 0; lua_rawsetp(L, LUA_REGISTRYINDEX, &kMyKey)` |
| 文件名 | `模块_library.{h,cc}` | `json_library.h`, `http_library.cc` |

---

## 8. 错误处理

```cpp
// 同步错误：使用 luaL_error（会抛出 Lua 错误，不返回）
if (invalid) {
    luaL_error(L, "invalid argument: expected string, got %s", lua_typename(L, lua_type(L, 1)));
    return 0;  // 不可达，但部分编译器需要
}

// 异步错误：通过 PushResume 返回错误值
rt->PushResume(handle, {
    LuaValue{(int64_t)0},                       // status = 0 表示失败
    LuaValue{nullptr},                           // body = nil
    LuaValue{std::string("connection refused")}  // err
});

// 库关闭检查
auto state = GetState(L);
if (!state || state->shutting_down.load()) {
    return luaL_error(L, "my library is shutting down");
}
```

---

## 9. 资源管理清单

- [ ] `Open()` 中创建的所有 registry 引用在 `Close()` 中清理
- [ ] Userdata 通过 metatable `__gc` 自动释放
- [ ] 异步操作捕获 `shared_ptr` 保持对象存活
- [ ] 库关闭时设置 `shutting_down` 原子标志，防止新的异步操作启动
- [ ] Lua 回调引用（`luaL_ref`）在不需要时调用 `luaL_unref` 释放
- [ ] `luaL_checkstack(L, N, nullptr)` 在深层递归前预留栈空间

---

## 10. 测试约定

测试文件放在 `tests/` 目录下，命名 `<module>_library_test.cc`。

```cpp
// tests/my_library_test.cc
#include <gtest/gtest.h>
#include "lua_runtime.h"
#include "my_library.h"

TEST(MyLibraryTest, BasicOperation) {
    auto rt = LuaRuntime::Builder()
        .RegisterLibrary(std::make_shared<MyLibrary>())
        .Create();

    auto result = rt->RunScript(R"lua(
        local my = require('my')
        return my.foo("hello")
    )lua").via([](auto&& x) { return std::move(x); }).get();

    EXPECT_EQ(result.status, LUA_OK);
}
```

---

## 11. 文件模板

### 头文件 (`my_library.h`)

```cpp
#pragma once

#include "lua_library.h"

class MyLibrary : public LuaLibrary {
public:
    std::string name() const override { return "my"; }
    void Open(lua_State* L) override;
    void Close(lua_State* L) override;  // 仅在需要清理时 override
};
```

### 实现文件 (`my_library.cc`)

```cpp
#include "my_library.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua_context.h"

namespace {

// 辅助函数、C Lua 函数定义...

static int my_foo(lua_State* L) {
    const char* input = luaL_checkstring(L, 1);
    // ...
    lua_pushstring(L, result.c_str());
    return 1;
}

}  // namespace

void MyLibrary::Open(lua_State* L) {
    lua_newtable(L);
    luaL_Reg funcs[] = {
        {"foo", my_foo},
        {nullptr, nullptr}
    };
    luaL_setfuncs(L, funcs, 0);
}

void MyLibrary::Close(lua_State* L) {
    // 清理资源（如果需要）
}
```
