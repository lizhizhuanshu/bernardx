#pragma once

#include <string>

#include <nlohmann/json.hpp>

// BT DSL (.bt) 文本 → 引擎 JSON 编译器。
//
// 1:1 镜像 bernard-agent2 的 bt_dsl/dsl.py（语法、语义、JSON 键序），产出与
// Python `compile_text()` 完全一致的树 JSON（Pipeline/Script/... + `*target`/
// `*timeout`/`*retry` 边参数），由既有 TreeParser::ParseNode 管线消费。
// 差分 golden 测试（tests/data/bt_dsl）保证两侧一致。
namespace bt_dsl {

// 编译结果：error 为空即成功（tree 为根节点 JSON 对象，键序与 Python 一致）。
struct DslResult {
    nlohmann::ordered_json tree;
    std::string error;  // "第 N 行: ..." 风格；registry 错误无行号前缀
};

// `.bt` 源文本 → 引擎 JSON。`registry_text`（可选）为完整 registry JSON 文本
// （整体替换内嵌默认词表）；nullptr → 使用 DefaultRegistryText()。
DslResult CompileText(const std::string& source,
                      const std::string* registry_text = nullptr);

// 路径是否以 .bt 结尾（唯一的 DSL 扩展名判定）。
bool IsBtPath(const std::string& path);

// 内嵌默认 registry（bernard-agent2 src/bernard_agent2/bt_dsl/registry.json
// 原文拷贝——两侧契约变更时须同步）。暴露给测试与替换基座。
const char* DefaultRegistryText();

}  // namespace bt_dsl
