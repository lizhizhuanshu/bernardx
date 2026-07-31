# LuaRuntime

基于 Lua 5.4 + async_simple 的异步 Lua 运行时，提供协程调度、HTTP/WebSocket 网络和 UE4/5 风格行为树。

## 项目结构

```
src/
├── lua/          # Lua 运行时核心（LuaContext, LuaRuntime, HttpLibrary）
└── bt/           # 行为树引擎（节点, 解析器, 黑板, BT Library）
tests/            # Google Test 测试
docs/             # 文档
```

## 文档

- [使用说明](docs/usage.md) — 构建、安装、运行、项目结构、Lua API 概览、依赖列表
- [Lua API 参考](docs/lua_api.md) — 全局函数、http 模块、bt 模块、require/loadfile
- [Lua Library 开发规范](docs/lua_library_guide.md) — 基类接口、Open/Close 模式、异步 yield/resume、metatable/userdata、命名约定
- [行为树](docs/bt.md) — bt.ready/bt.exec 生命周期（root/sensor_defs 为 JSON 文件路径，`@`资源或绝对路径；Subtree 按 path 加载）、节点类型、装饰器、传感器、完整示例
- [运行时核心架构](docs/architecture.md) — 组件关系、require 模块解析、异步加载时序（mermaid）

## 构建

```bash
cmake -B build
cmake --build build
```

## 运行测试

```bash
cd build && ./lua_runtime_test
```
