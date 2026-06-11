# Lua API 参考

## 模块概览

| 模块 | 加载方式 | 说明 |
|------|---------|------|
| 全局内建 | 无需 require | `now` / `sleep` / `setTimeout` / `clearTimeout` / `await` |
| `http` | `require('http')` | HTTP 客户端 + WebSocket |
| `blackboard` | `require('blackboard')` | 黑板键值存储（线程安全） |
| `bt` | `require('bt')` | 行为树（见 [bt_node_config.md](bt_node_config.md)） |
| `async` | `require('async')` | 异步文件 I/O 和进程管理 |

## 详细文档

| 模块 | 文档 |
|------|------|
| 全局内建函数 | [api/globals.md](api/globals.md) |
| http 模块 | [api/http.md](api/http.md) |
| require / loadfile | [api/require.md](api/require.md) |
| blackboard 模块 | [api/blackboard.md](api/blackboard.md) |
| bt 模块 | [api/bt.md](api/bt.md) |
| async 模块 | [api/async.md](api/async.md) |
