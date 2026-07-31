# bernardx — 命令行使用说明

## 语法

```
bernardx [--dir=目录] [--entry=入口文件]
```

## 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--dir` | `.` (当前目录) | 工作目录，需包含 `src/` 和 `libs/` |
| `--entry` | `src/main.lua` | 入口 Lua 文件，相对于 `--dir` |
| `--help` | — | 显示帮助信息（gflags 内置） |

## 说明

`bernardx` 启动后会：

1. 解析 `--dir` 参数，将其作为工作目录
2. 创建 `FileSystemCodeProvider`，在 `src/` 和 `libs/` 子目录中查找 Lua 模块
3. 初始化 Lua 运行时（注册 http、json、bt、blackboard 四个内置库）
4. 执行 `{dir}/{entry}` 指定的入口文件
5. 脚本执行完毕后，如果行为树仍在运行，等待其结束后退出

## 示例

```bash
# 在当前目录运行（默认入口 src/main.lua）
bernardx

# 指定项目目录
bernardx --dir=/path/to/my_project

# 指定入口文件
bernardx --entry=src/app.lua
bernardx --dir=/path/to/project --entry=scripts/init.lua

# 显示帮助
bernardx --help
```

## 工作目录结构

```
my_project/               ← --dir 指向这里
├── src/
│   ├── main.lua          # 默认入口脚本
│   └── *.lua             # 其他模块
├── libs/                 # 第三方 Lua 库
├── scripts/              # BT Script 节点脚本（由 CodeProvider 加载）
└── sensors/              # BT 传感器脚本（由 CodeProvider 加载）
```

行为树结构在 `bt.ready` 调用时以 Lua table 直接传入（`root` / `subtrees` / `sensor_defs`），不经文件或 JSON。Script 节点与传感器的 Lua 脚本仍由 `path` 指定，统一由运行时 `CodeProvider` 加载（与主脚本 `require()` 共用同一加载器）——请将 `scripts/`、`sensors/` 等脚本目录纳入 `CodeProvider` 搜索路径。详见 [行为树文档](bt.md)。

## 退出码

| 退出码 | 说明 |
|--------|------|
| 0 | 正常退出 |
| 1 | 目录不存在，入口文件不存在，或脚本执行失败 |
