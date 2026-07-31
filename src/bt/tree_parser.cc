#include "tree_parser.h"

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

#include "blackboard_condition.h"
#include "composite.h"
#include "decorator.h"
#include "force_failure.h"
#include "force_success.h"
#include "inverter.h"
#include "lua_types.h"
#include "node.h"
#include "parallel.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "repeat.h"
#include "resume_sequence.h"
#include "retry_until_successful.h"
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
    const json* sensor_defs = nullptr;            // {name: {interval, path, params}}
    std::set<std::string> resolving;              // subtree path cycle guard
    uint32_t next_id = 1;
    std::string error;
};

void SetError(ParseContext& ctx, std::string msg) {
    spdlog::error("TreeParser: {}", msg);
    ctx.error = std::move(msg);
}

AbortMode ParseAbortMode(const std::string& s) {
    if (s == "Self") return AbortMode::kSelf;
    if (s == "LowerPriority") return AbortMode::kLowerPriority;
    if (s == "Both") return AbortMode::kBoth;
    return AbortMode::kNone;
}

Parallel::Policy ParseParallelPolicy(const std::string& s) {
    if (s == "RequireOne") return Parallel::Policy::kRequireOne;
    return Parallel::Policy::kRequireAll;
}

std::optional<LuaValue> ParseLuaValue(const json& j) {
    if (j.is_null()) return std::nullopt;
    if (j.is_boolean()) return LuaValue(j.get<bool>());
    if (j.is_number_integer()) return LuaValue(static_cast<int64_t>(j.get<int64_t>()));
    if (j.is_number_float()) return LuaValue(j.get<double>());
    if (j.is_string()) return LuaValue(j.get<std::string>());
    spdlog::warn("TreeParser: unsupported value type for blackboard condition");
    return std::nullopt;
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

void ApplyDecorators(const json& j, Node* node);
void ApplySensors(const json& j, Node* node, ParseContext& ctx);

void ApplyDecorators(const json& j, Node* node) {
    if (!j.contains("decorators") || !j["decorators"].is_array()) return;

    for (const auto& dec_j : j["decorators"]) {
        if (!dec_j.is_object() || !dec_j.contains("type")) continue;
        std::string dec_type = dec_j["type"].get<std::string>();
        auto abort = ParseAbortMode(dec_j.value("abort", "None"));

        if (dec_type == "BlackboardCondition") {
            if (!dec_j.contains("key") || !dec_j["key"].is_string()) {
                spdlog::warn("TreeParser: BlackboardCondition missing 'key'");
                continue;
            }
            std::string key = dec_j["key"].get<std::string>();
            std::string op = dec_j.value("operator", "is_set");
            std::optional<LuaValue> expected;
            if (dec_j.contains("value")) expected = ParseLuaValue(dec_j["value"]);
            node->AddDecorator(std::make_unique<BlackboardCondition>(
                std::move(key), std::move(op), std::move(expected), abort));
        } else if (dec_type == "Inverter") {
            auto dec = std::make_unique<Inverter>(abort);
            if (dec_j.contains("child") && dec_j["child"].is_object()) {
                const auto& child_j = dec_j["child"];
                if (child_j.contains("type") && child_j["type"].is_string()) {
                    std::string child_type = child_j["type"].get<std::string>();
                    if (child_type == "BlackboardCondition" && child_j.contains("key") &&
                        child_j["key"].is_string()) {
                        std::string ckey = child_j["key"].get<std::string>();
                        std::string cop = child_j.value("operator", "is_set");
                        std::optional<LuaValue> cexpected;
                        if (child_j.contains("value")) cexpected = ParseLuaValue(child_j["value"]);
                        dec->set_child(std::make_unique<BlackboardCondition>(
                            std::move(ckey), std::move(cop), std::move(cexpected), abort));
                    } else if (child_type == "ForceSuccess") {
                        dec->set_child(std::make_unique<ForceSuccess>(abort));
                    } else if (child_type == "ForceFailure") {
                        dec->set_child(std::make_unique<ForceFailure>(abort));
                    }
                }
            }
            node->AddDecorator(std::move(dec));
        } else if (dec_type == "ForceSuccess") {
            node->AddDecorator(std::make_unique<ForceSuccess>(abort));
        } else if (dec_type == "ForceFailure") {
            node->AddDecorator(std::make_unique<ForceFailure>(abort));
        } else {
            spdlog::warn("TreeParser: unknown decorator type '{}'", dec_type);
        }
    }
}

void ApplySensors(const json& j, Node* node, ParseContext& ctx) {
    if (!j.contains("sensors") || !j["sensors"].is_array()) return;

    for (const auto& sen_j : j["sensors"]) {
        if (!sen_j.is_string()) continue;
        std::string name = sen_j.get<std::string>();

        SensorSpec spec;
        spec.name = name;
        spec.interval_ms = 100;
        if (ctx.sensor_defs && ctx.sensor_defs->contains(name) &&
            (*ctx.sensor_defs)[name].is_object()) {
            const auto& def = (*ctx.sensor_defs)[name];
            spec.description = def.value("description", "");
            spec.interval_ms = def.value("interval", static_cast<int64_t>(100));
            spec.script_path = def.value("path", "");
            if (def.contains("params") && def["params"].is_object()) {
                for (auto it = def["params"].begin(); it != def["params"].end(); ++it) {
                    if (auto v = ParseLuaValue(it.value())) {
                        spec.args[it.key()] = std::move(*v);
                    }
                }
            }
        }
        if (spec.script_path.empty()) {
            SetError(ctx, "sensor '" + name + "' is not defined or missing 'path'");
            return;
        }
        node->AddSensorSpec(std::move(spec));
    }
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
    } else if (type == "ResumeSequence") {
        node = std::make_unique<ResumeSequence>(id, name);
    } else if (type == "Parallel") {
        auto sp = j.value("success_policy", "RequireAll");
        auto fp = j.value("failure_policy", "RequireOne");
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

    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get(), ctx);
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseScriptLeaf(const json& j, ParseContext& ctx) {
    if (!j.contains("path") || !j["path"].is_string()) {
        SetError(ctx, "Script node missing 'path' field");
        co_return nullptr;
    }
    std::string path = j["path"].get<std::string>();
    std::string name = j.value("name", path);
    ScriptNode::ArgsMap args;
    if (j.contains("params") && j["params"].is_object()) {
        for (auto it = j["params"].begin(); it != j["params"].end(); ++it) {
            if (auto v = ParseLuaValue(it.value())) args[it.key()] = std::move(*v);
        }
    }
    uint32_t id = ctx.next_id++;

    auto node = std::make_unique<ScriptNode>(id, std::move(name), std::move(path), std::move(args));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get(), ctx);
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseSubtree(const json& j, ParseContext& ctx) {
    if (!j.contains("path") || !j["path"].is_string()) {
        SetError(ctx, "Subtree node missing 'path' field");
        co_return nullptr;
    }
    std::string path = j["path"].get<std::string>();

    if (ctx.resolving.count(path)) {
        SetError(ctx, "circular subtree reference '" + path + "'");
        co_return nullptr;
    }
    if (!ctx.provider) {
        // Sync Parse() entry has no loader.
        SetError(ctx, "subtree loading unavailable for '" + path + "'");
        co_return nullptr;
    }

    auto content = co_await LoadAsset(path, ctx.provider);
    if (!content) {
        SetError(ctx, "failed to load subtree '" + path + "'");
        co_return nullptr;
    }
    json sub_j;
    try {
        sub_j = json::parse(*content);
    } catch (const json::parse_error& e) {
        SetError(ctx, std::string("failed to parse subtree '") + path + "': " + e.what());
        co_return nullptr;
    }

    ctx.resolving.insert(path);
    auto sub_root = co_await ParseNode(sub_j, ctx);
    ctx.resolving.erase(path);

    if (!sub_root) {
        if (ctx.error.empty()) SetError(ctx, "failed to parse subtree '" + path + "'");
        co_return nullptr;
    }

    std::string name = j.value("name", path);
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<SubtreeNode>(id, std::move(name), std::move(path), std::move(sub_root));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get(), ctx);
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
    int count = j.value("count", Repeat::kInfinite);
    std::string name = j.value("name", "Repeat");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<Repeat>(id, std::move(name), count, std::move(child));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get(), ctx);
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseRetry(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "RetryUntilSuccessful node requires at least one child");
    if (!child) co_return nullptr;
    int attempts = j.value("attempts", RetryUntilSuccessful::kInfinite);
    std::string name = j.value("name", "RetryUntilSuccessful");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<RetryUntilSuccessful>(id, std::move(name), attempts, std::move(child));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get(), ctx);
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseWait(const json& j, ParseContext& ctx) {
    int ms = j.value("ms", 1000);
    std::string name = j.value("name", "Wait");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<WaitNode>(id, std::move(name), ms);
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get(), ctx);
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

    if (type == "Selector" || type == "Sequence" || type == "ResumeSequence" ||
        type == "Parallel" || type == "RandomSelector" || type == "RandomSequence") {
        co_return co_await ParseComposite(j, ctx);
    }
    if (type == "Script") co_return co_await ParseScriptLeaf(j, ctx);
    if (type == "Subtree") co_return co_await ParseSubtree(j, ctx);
    if (type == "Repeat") co_return co_await ParseRepeat(j, ctx);
    if (type == "RetryUntilSuccessful") co_return co_await ParseRetry(j, ctx);
    if (type == "Wait") co_return co_await ParseWait(j, ctx);

    SetError(ctx, "unknown node type '" + type + "'");
    co_return nullptr;
}

}  // namespace

async_simple::coro::Lazy<ParseResult>
TreeParser::LoadAndParse(const std::string& root_path,
                         const std::string& sensor_defs_path,
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

    json sensor_defs_j = json::object();
    if (!sensor_defs_path.empty()) {
        auto sd_content = co_await LoadAsset(sensor_defs_path, provider);
        if (!sd_content) {
            result.error = "failed to load sensor_defs '" + sensor_defs_path + "'";
            spdlog::error("TreeParser: {}", result.error);
            co_return result;
        }
        try {
            sensor_defs_j = json::parse(*sd_content);
        } catch (const json::parse_error& e) {
            result.error = std::string("failed to parse sensor_defs '") + sensor_defs_path + "': " + e.what();
            spdlog::error("TreeParser: {}", result.error);
            co_return result;
        }
        if (!sensor_defs_j.is_object()) {
            result.error = "sensor_defs '" + sensor_defs_path + "' must be a JSON object";
            spdlog::error("TreeParser: {}", result.error);
            co_return result;
        }
    }

    ParseContext ctx;
    ctx.provider = provider;
    ctx.sensor_defs = &sensor_defs_j;

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
