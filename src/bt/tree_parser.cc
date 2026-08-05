#include "tree_parser.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include "resource_provider.h"

#include "composite.h"
#include "condition_composite.h"
#include "force_failure.h"
#include "force_success.h"
#include "inverter.h"
#include "lua_types.h"
#include "node.h"
#include "node_condition.h"
#include "parallel.h"
#include "pipeline.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "repeat.h"
#include "retry_until_successful.h"
#include "script_condition.h"
#include "script_node.h"
#include "selector.h"
#include "sequence.h"
#include "subtree_node.h"
#include "types.h"
#include "wait_node.h"

using json = nlohmann::json;

namespace {

struct ParseContext {
    std::shared_ptr<ResourceProvider> provider;   // null in sync Parse -> Subtree unsupported
    std::set<std::string> resolving;              // subtree path cycle guard
    uint32_t next_id = 1;
    std::string error;
};

void SetError(ParseContext& ctx, std::string msg) {
    spdlog::error("TreeParser: {}", msg);
    ctx.error = std::move(msg);
}

Parallel::Policy ParseParallelPolicy(const std::string& s) {
    if (s == "RequireOne") return Parallel::Policy::kRequireOne;
    return Parallel::Policy::kRequireAll;
}

// --- Subtree param templating ---
//
// A Subtree node may carry a `params` object whose values are forwarded into
// the wrapped subtree JSON by substituting {{key}} placeholders that appear
// in ANY string field — params, source, name, condition fields, and nested
// Subtree nodes alike. A placeholder that is the entire value ("{{age}}") is
// replaced by the param's original JSON value so the type is preserved (number
// stays number, bool stays bool, object stays object); a placeholder embedded
// in a larger string ("hello {{name}}") is interpolated as text. Unknown keys
// are left literal with a single warning. Nested Subtrees propagate
// transparently: outer params template the inner Subtree node (its source and
// params), which then template their own subtree JSON, and so on.

std::string TrimWs(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// If `s` is exactly one placeholder "{{ key }}" (surrounding whitespace ok,
// no inner braces), write the trimmed key and return true.
bool MatchWholePlaceholder(const std::string& s, std::string& key) {
    std::string t = TrimWs(s);
    if (t.size() < 5) return false;  // minimum "{{x}}"
    if (t.compare(0, 2, "{{") != 0) return false;
    if (t.compare(t.size() - 2, 2, "}}") != 0) return false;
    std::string inner = t.substr(2, t.size() - 4);
    if (inner.find("{{") != std::string::npos) return false;
    if (inner.find("}}") != std::string::npos) return false;
    key = TrimWs(inner);
    return true;
}

// Text form of a param value for embedding inside a larger string.
std::string StringifyParam(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_float()) return v.dump();
    if (v.is_null()) return "";
    return v.dump();
}

// Replace every {{ key }} occurrence in `s` using `params`. Unknown keys are
// left literal (with a single warning per occurrence).
std::string SubstitutePartial(const std::string& s, const json& params) {
    std::string out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t open = s.find("{{", pos);
        if (open == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        size_t close = s.find("}}", open + 2);
        if (close == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, open - pos);
        std::string key = TrimWs(s.substr(open + 2, close - (open + 2)));
        if (!key.empty() && params.contains(key)) {
            out.append(StringifyParam(params[key]));
        } else {
            out.append(s, open, close + 2 - open);  // keep literal
            if (!key.empty()) {
                spdlog::warn("TreeParser: subtree param '{}' not provided", key);
            }
        }
        pos = close + 2;
    }
    return out;
}

// Recursively walk `target` and substitute {{key}} placeholders in every
// string value (any field) using `params`. Whole-value placeholders preserve
// the param's type; embedded placeholders are interpolated as text.
void SubstituteTemplates(json& target, const json& params) {
    if (target.is_object() || target.is_array()) {
        for (auto& el : target) SubstituteTemplates(el, params);
        return;
    }
    if (!target.is_string()) return;
    std::string s = target.get<std::string>();
    std::string key;
    if (MatchWholePlaceholder(s, key)) {
        if (!key.empty() && params.contains(key)) {
            target = params[key];  // type preserved (incl. objects/arrays)
        } else if (!key.empty()) {
            spdlog::warn("TreeParser: subtree param '{}' not provided", key);
        }
        return;
    }
    target = SubstitutePartial(s, params);
}

// Read a scalar knob from the node's `params` object (e.g. Wait.ms,
// Repeat.count, Parallel.success_policy). Returns `def` if absent/wrong type.
std::string ParamsString(const json& j, const char* key, std::string def) {
    if (j.contains("params") && j["params"].is_object() &&
        j["params"].contains(key) && j["params"][key].is_string()) {
        return j["params"][key].get<std::string>();
    }
    return def;
}

int ParamsInt(const json& j, const char* key, int def) {
    if (j.contains("params") && j["params"].is_object() &&
        j["params"].contains(key) && j["params"][key].is_number_integer()) {
        return j["params"][key].get<int>();
    }
    return def;
}

// Load a JSON asset by path. "@rel" -> ResourceProvider; otherwise absolute read.
async_simple::coro::Lazy<std::optional<std::string>>
LoadAsset(const std::string& path, std::shared_ptr<ResourceProvider> provider) {
    if (!path.empty() && path[0] == '@') {
        if (!provider) {
            spdlog::error("TreeParser: no resource provider for '@' path '{}'", path);
            co_return std::nullopt;
        }
        co_return co_await provider->Load(path.substr(1));
    }
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) co_return std::nullopt;
    co_return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

void ReadDescription(const json& j, Node* node) {
    if (j.contains("description") && j["description"].is_string()) {
        node->set_description(j["description"].get<std::string>());
    }
}

// Forward declarations
async_simple::coro::Lazy<std::unique_ptr<Node>> ParseNode(const json& j, ParseContext& ctx);
async_simple::coro::Lazy<std::unique_ptr<NodeCondition>> ParseCondition(const json& j, ParseContext& ctx);

async_simple::coro::Lazy<bool> ApplyCondition(const json& j, Node* node, ParseContext& ctx);

// Build a condition tree from a `condition` JSON object. Shapes:
//   {"type":"Script","source":...,"params":{...}}
//   {"type":"And"/"Or","children":[<condition>,...]}
//   {"type":"Not","child":<condition>}
async_simple::coro::Lazy<std::unique_ptr<NodeCondition>>
ParseCondition(const json& j, ParseContext& ctx) {
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        SetError(ctx, "condition must be a JSON object with a 'type'");
        co_return nullptr;
    }
    std::string type = j["type"].get<std::string>();

    if (type == "Script") {
        if (!j.contains("source") || !j["source"].is_string()) {
            SetError(ctx, "Script condition missing 'source'");
            co_return nullptr;
        }
        std::string source = j["source"].get<std::string>();
        std::string name = j.value("name", source);
        json params = j.value("params", json::object());
        co_return std::make_unique<ScriptCondition>(std::move(name), std::move(source),
                                                     std::move(params));
    }

    if (type == "And" || type == "Or") {
        std::unique_ptr<NodeCondition> node =
            (type == "And") ? std::unique_ptr<NodeCondition>(std::make_unique<AndCondition>())
                            : std::unique_ptr<NodeCondition>(std::make_unique<OrCondition>());
        if (j.contains("children") && j["children"].is_array()) {
            for (const auto& cj : j["children"]) {
                auto child = co_await ParseCondition(cj, ctx);
                if (!child) co_return nullptr;
                if (type == "And") {
                    static_cast<AndCondition*>(node.get())->AddChild(std::move(child));
                } else {
                    static_cast<OrCondition*>(node.get())->AddChild(std::move(child));
                }
            }
        }
        co_return node;
    }

    if (type == "Not") {
        if (!j.contains("child") || !j["child"].is_object()) {
            SetError(ctx, "Not condition requires a 'child' object");
            co_return nullptr;
        }
        auto child = co_await ParseCondition(j["child"], ctx);
        if (!child) co_return nullptr;
        co_return std::make_unique<NotCondition>(std::move(child));
    }

    SetError(ctx, "unknown condition type '" + type + "'");
    co_return nullptr;
}

AbortMode ParseAbortMode(const std::string& s) {
    if (s == "Self") return AbortMode::kSelf;
    if (s == "LowerPriority") return AbortMode::kLowerPriority;
    if (s == "Both") return AbortMode::kBoth;
    return AbortMode::kNone;
}

// Attach a `condition` (if present) to a node. The optional `abort` field on
// the condition object selects its reactive abort mode (default None). Returns
// false on parse error.
async_simple::coro::Lazy<bool> ApplyCondition(const json& j, Node* node, ParseContext& ctx) {
    if (!j.contains("condition") || !j["condition"].is_object()) co_return true;
    auto cond = co_await ParseCondition(j["condition"], ctx);
    if (!cond) co_return false;
    if (j["condition"].contains("abort") && j["condition"]["abort"].is_string()) {
        cond->set_abort(ParseAbortMode(j["condition"]["abort"].get<std::string>()));
    }
    node->SetCondition(std::shared_ptr<NodeCondition>(cond.release()));
    co_return true;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseComposite(const json& j, ParseContext& ctx) {
    std::string type = j["type"].get<std::string>();
    std::string name = j.value("name", type);
    uint32_t id = ctx.next_id++;

    std::unique_ptr<Node> node;
    if (type == "Selector") {
        node = std::make_unique<Selector>(id, name);
    } else if (type == "Sequence") {
        node = std::make_unique<Sequence>(id, name);
    } else if (type == "Parallel") {
        auto sp = ParamsString(j, "success_policy", "RequireAll");
        auto fp = ParamsString(j, "failure_policy", "RequireOne");
        node = std::make_unique<Parallel>(id, name, ParseParallelPolicy(sp), ParseParallelPolicy(fp));
    } else if (type == "RandomSelector") {
        node = std::make_unique<RandomSelector>(id, name);
    } else if (type == "RandomSequence") {
        node = std::make_unique<RandomSequence>(id, name);
    }
    if (!node) {
        SetError(ctx, "failed to build composite node '" + type + "'");
        co_return nullptr;
    }

    if (j.contains("children") && j["children"].is_array()) {
        for (const auto& child_j : j["children"]) {
            auto child = co_await ParseNode(child_j, ctx);
            if (!child) co_return nullptr;
            static_cast<Composite*>(node.get())->AddChild(std::move(child));
        }
    }

    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

// Pipeline: scan-to-start + wait/retry composite. Each child is a normal node
// (built by ParseNode) and may carry its own `condition` guard (set via
// ApplyCondition inside ParseNode's helpers) plus two Pipeline edge params:
//   `$timeout` (integer TICKS) — how many ticks to wait for this step's
//                                condition (0 / absent = wait forever);
//   `$retry`   (integer)       — max times to back up and re-run the previous
//                                step's action when this wait times out.
// These `$`-prefixed keys are intentionally distinct from node-own fields
// (source/condition/params); the node itself ignores them, the Pipeline reads
// them here. The Pipeline node may also be guarded, so nested pipelines work.
async_simple::coro::Lazy<std::unique_ptr<Node>> ParsePipeline(const json& j, ParseContext& ctx) {
    std::string name = j.value("name", "Pipeline");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<Pipeline>(id, name);

    if (!j.contains("children") || !j["children"].is_array() || j["children"].empty()) {
        SetError(ctx, "Pipeline node requires at least one child");
        co_return nullptr;
    }
    for (const auto& child_j : j["children"]) {
        if (!child_j.is_object()) {
            SetError(ctx, "Pipeline child must be a JSON object");
            co_return nullptr;
        }
        auto child = co_await ParseNode(child_j, ctx);
        if (!child) co_return nullptr;
        int timeout = 0;
        if (child_j.contains("$timeout") && child_j["$timeout"].is_number_integer()) {
            timeout = child_j["$timeout"].get<int>();
            if (timeout < 0) timeout = 0;
        }
        int retry = 0;
        if (child_j.contains("$retry") && child_j["$retry"].is_number_integer()) {
            retry = child_j["$retry"].get<int>();
            if (retry < 0) retry = 0;
        }
        node->AddStep(std::move(child), timeout, retry);
    }

    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseScriptLeaf(const json& j, ParseContext& ctx) {
    if (!j.contains("source") || !j["source"].is_string()) {
        SetError(ctx, "Script node missing 'source' field");
        co_return nullptr;
    }
    std::string source = j["source"].get<std::string>();
    std::string name = j.value("name", source);
    // Params are stored raw and resolved to LuaValues at Init (tables need a
    // lua_State, which doesn't exist at parse time).
    json params = j.value("params", json::object());
    uint32_t id = ctx.next_id++;

    auto node = std::make_unique<ScriptNode>(id, std::move(name), std::move(source), std::move(params));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseSubtree(const json& j, ParseContext& ctx) {
    if (!j.contains("source") || !j["source"].is_string()) {
        SetError(ctx, "Subtree node missing 'source' field");
        co_return nullptr;
    }
    std::string source = j["source"].get<std::string>();

    if (ctx.resolving.count(source)) {
        SetError(ctx, "circular subtree reference '" + source + "'");
        co_return nullptr;
    }
    if (!ctx.provider) {
        // Sync Parse() entry has no loader.
        SetError(ctx, "subtree loading unavailable for '" + source + "'");
        co_return nullptr;
    }

    auto content = co_await LoadAsset(source, ctx.provider);
    if (!content) {
        SetError(ctx, "failed to load subtree '" + source + "'");
        co_return nullptr;
    }
    json sub_j;
    try {
        sub_j = json::parse(*content);
    } catch (const json::parse_error& e) {
        SetError(ctx, std::string("failed to parse subtree '") + source + "': " + e.what());
        co_return nullptr;
    }

    // Forward this Subtree node's `params` into the subtree JSON by
    // substituting {{key}} placeholders in every string field (type-preserving
    // for whole-value placeholders; string interpolation otherwise). No
    // `params` -> no substitution (opt-in).
    if (j.contains("params") && j["params"].is_object() && !j["params"].empty()) {
        SubstituteTemplates(sub_j, j["params"]);
    }

    ctx.resolving.insert(source);
    auto sub_root = co_await ParseNode(sub_j, ctx);
    ctx.resolving.erase(source);

    if (!sub_root) {
        if (ctx.error.empty()) SetError(ctx, "failed to parse subtree '" + source + "'");
        co_return nullptr;
    }

    std::string name = j.value("name", source);
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<SubtreeNode>(id, std::move(name), std::move(source), std::move(sub_root));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>>
ParseSingleChild(const json& j, ParseContext& ctx, const char* missing_msg) {
    if (j.contains("children") && j["children"].is_array() && !j["children"].empty() &&
        j["children"][0].is_object()) {
        co_return co_await ParseNode(j["children"][0], ctx);
    }
    if (ctx.error.empty()) SetError(ctx, missing_msg);
    co_return nullptr;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseRepeat(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "Repeat node requires at least one child");
    if (!child) co_return nullptr;
    int count = ParamsInt(j, "count", Repeat::kInfinite);
    std::string name = j.value("name", "Repeat");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<Repeat>(id, std::move(name), count, std::move(child));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseRetry(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "RetryUntilSuccessful node requires at least one child");
    if (!child) co_return nullptr;
    int attempts = ParamsInt(j, "attempts", RetryUntilSuccessful::kInfinite);
    std::string name = j.value("name", "RetryUntilSuccessful");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<RetryUntilSuccessful>(id, std::move(name), attempts, std::move(child));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseWait(const json& j, ParseContext& ctx) {
    int ms = ParamsInt(j, "ms", 1000);
    std::string name = j.value("name", "Wait");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<WaitNode>(id, std::move(name), ms);
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseForceSuccess(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "ForceSuccess node requires at least one child");
    if (!child) co_return nullptr;
    std::string name = j.value("name", "ForceSuccess");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<ForceSuccess>(id, std::move(name), std::move(child));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseForceFailure(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "ForceFailure node requires at least one child");
    if (!child) co_return nullptr;
    std::string name = j.value("name", "ForceFailure");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<ForceFailure>(id, std::move(name), std::move(child));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseInverter(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "Inverter node requires at least one child");
    if (!child) co_return nullptr;
    std::string name = j.value("name", "Inverter");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<Inverter>(id, std::move(name), std::move(child));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseNode(const json& j, ParseContext& ctx) {
    if (!j.is_object()) {
        SetError(ctx, "node definition must be a JSON object");
        co_return nullptr;
    }
    if (!j.contains("type") || !j["type"].is_string()) {
        SetError(ctx, "node missing 'type' field");
        co_return nullptr;
    }
    std::string type = j["type"].get<std::string>();

    if (type == "Selector" || type == "Sequence" ||
        type == "Parallel" || type == "RandomSelector" || type == "RandomSequence") {
        co_return co_await ParseComposite(j, ctx);
    }
    if (type == "Pipeline") co_return co_await ParsePipeline(j, ctx);
    if (type == "Script") co_return co_await ParseScriptLeaf(j, ctx);
    if (type == "Subtree") co_return co_await ParseSubtree(j, ctx);
    if (type == "Repeat") co_return co_await ParseRepeat(j, ctx);
    if (type == "RetryUntilSuccessful") co_return co_await ParseRetry(j, ctx);
    if (type == "Wait") co_return co_await ParseWait(j, ctx);
    if (type == "ForceSuccess") co_return co_await ParseForceSuccess(j, ctx);
    if (type == "ForceFailure") co_return co_await ParseForceFailure(j, ctx);
    if (type == "Inverter") co_return co_await ParseInverter(j, ctx);

    SetError(ctx, "unknown node type '" + type + "'");
    co_return nullptr;
}

}  // namespace

async_simple::coro::Lazy<ParseResult>
TreeParser::LoadAndParse(const std::string& root_path,
                         std::shared_ptr<ResourceProvider> provider) {
    ParseResult result;

    auto root_content = co_await LoadAsset(root_path, provider);
    if (!root_content) {
        result.error = "failed to load root '" + root_path + "'";
        spdlog::error("TreeParser: {}", result.error);
        co_return result;
    }
    json root_j;
    try {
        root_j = json::parse(*root_content);
    } catch (const json::parse_error& e) {
        result.error = std::string("failed to parse root '") + root_path + "': " + e.what();
        spdlog::error("TreeParser: {}", result.error);
        co_return result;
    }
    if (!root_j.is_object()) {
        result.error = "root '" + root_path + "' must be a JSON object";
        spdlog::error("TreeParser: {}", result.error);
        co_return result;
    }

    ParseContext ctx;
    ctx.provider = provider;

    auto root = co_await ParseNode(root_j, ctx);
    if (!root) {
        result.error = ctx.error.empty() ? "failed to parse tree" : ctx.error;
        spdlog::error("TreeParser: {}", result.error);
        co_return result;
    }
    result.root = std::move(root);
    co_return result;
}

ParseResult TreeParser::Parse(const std::string& root_json) {
    ParseResult result;
    json root_j;
    try {
        root_j = json::parse(root_json);
    } catch (const json::parse_error& e) {
        result.error = std::string("JSON parse error: ") + e.what();
        spdlog::error("TreeParser: {}", result.error);
        return result;
    }
    if (!root_j.is_object()) {
        result.error = "root must be a JSON object";
        spdlog::error("TreeParser: {}", result.error);
        return result;
    }

    ParseContext ctx;  // no provider -> Subtree nodes unsupported
    auto root = async_simple::coro::syncAwait(ParseNode(root_j, ctx));
    if (!root) {
        result.error = ctx.error.empty() ? "failed to parse tree" : ctx.error;
        spdlog::error("TreeParser: {}", result.error);
        return result;
    }
    result.root = std::move(root);
    return result;
}
