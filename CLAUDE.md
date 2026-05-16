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
- [行为树节点 JSON 配置](docs/bt_node_config.md) — 节点类型、装饰器、目录模式、完整示例
- [行为树目录结构规范](docs/bt_directory_structure.md) — trees/、scripts/、sensors/ 目录组织方式

## 构建

```bash
cmake -B build
cmake --build build
```

## 运行测试

```bash
cd build && ./lua_runtime_test
```
