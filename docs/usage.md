# bernardx — 命令行使用说明

## 语法

```
bernardx [--dir=目录]
```

## 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--dir` | `.` (当前目录) | 工作目录，需包含 `src/main.lua` |
| `--help` | — | 显示帮助信息（gflags 内置） |

## 说明

`bernardx` 启动后会：

1. 解析 `--dir` 参数，将其作为工作目录
2. 创建 `FileSystemCodeProvider`，在 `src/` 和 `libs/` 子目录中查找 Lua 模块
3. 初始化 Lua 运行时（注册 http、json、bt 三个内置库）
4. 执行 `{dir}/src/main.lua`
5. 脚本执行完毕后，如果行为树仍在运行，等待其结束后退出

## 示例

```bash
# 在当前目录运行（需要 ./src/main.lua 存在）
bernardx

# 指定项目目录
bernardx --dir=/path/to/my_project

# 显示帮助
bernardx --help
```

## 工作目录结构

```
my_project/               ← --dir 指向这里
├── src/
│   ├── main.lua          # 入口脚本（必需）
│   └── *.lua             # 其他模块
├── libs/                 # 第三方 Lua 库
├── trees/                # 行为树 JSON
├── scripts/              # 行为树脚本节点
└── sensors/              # 传感器脚本
```

## 退出码

| 退出码 | 说明 |
|--------|------|
| 0 | 正常退出 |
| 1 | 目录不存在，或 main.lua 执行失败 |
