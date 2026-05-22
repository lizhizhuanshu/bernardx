# 包装节点 (Wrapper)

包装单个子节点，修改其执行行为。通过 `"children"` 数组配置（仅取第一个子节点）。

---

## Repeat - 重复节点

重复执行子节点，直到达到指定次数或子节点失败。

```json
{
  "type": "Repeat",
  "count": 3,
  "children": [
    {"type": "Script", "path": "a.lua"}
  ]
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `count` | 否 | 重复次数，`-1` 表示无限重复（默认 `-1`） |
| `children` | **是** | 子节点数组（仅使用第一个） |

**行为：**
- **有限次数**：子节点每成功一次计数 +1，达到 count 返回 Success；子节点失败则立即返回 Failure
- **无限重复**（`count: -1`）：子节点成功后重置并继续，子节点失败才返回 Failure
- 子节点 Running 时返回 Running

---

## RetryUntilSuccessful - 重试节点

子节点失败时自动重试，直到成功或达到最大尝试次数。

```json
{
  "type": "RetryUntilSuccessful",
  "attempts": 5,
  "children": [
    {"type": "Script", "path": "retry_action.lua"}
  ]
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `attempts` | 否 | 最大尝试次数，`-1` 表示无限重试（默认 `-1`） |
| `children` | **是** | 子节点数组（仅使用第一个） |

**行为：** 子节点成功则立即返回 Success；失败时重置子节点并重试，超过最大次数返回 Failure。子节点 Running 时返回 Running。
