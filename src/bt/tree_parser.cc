#include "tree_parser.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "blackboard_condition.h"
#include "composite.h"
#include "decorator.h"
#include "force_failure.h"
#include "force_success.h"
#include "inverter.h"
#include "leaf.h"
#include "node.h"
#include "parallel.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "repeat.h"
#include "retry_until_successful.h"
#include "script_node.h"
#include "selector.h"
#include "sequence.h"
#include "subtree_node.h"
#include "wait_node.h"

namespace {

AbortMode ParseAbortMode(const std::string& s) {
    if (s == "Self") return AbortMode::kSelf;
    if (s == "LowerPriority") return AbortMode::kLowerPriority;
    if (s == "Both") return AbortMode::kBoth;
    return AbortMode::kNone;
}

std::optional<LuaValue> ParseLuaValue(const nlohmann::json& j) {
    if (j.is_null()) return std::nullopt;
    if (j.is_boolean()) return LuaValue(j.get<bool>());
    if (j.is_number_integer()) return LuaValue(static_cast<int64_t>(j.get<int64_t>()));
    if (j.is_number_float()) return LuaValue(j.get<double>());
    if (j.is_string()) return LuaValue(j.get<std::string>());
    spdlog::warn("TreeParser: unsupported value type for blackboard condition");
    return std::nullopt;
}

Parallel::Policy ParseParallelPolicy(const std::string& s) {
    if (s == "RequireOne") return Parallel::Policy::kRequireOne;
    return Parallel::Policy::kRequireAll;
}
}  // namespace

std::string TreeParser::LoadTreeFromDirectory(const std::string& dir_path) {
    namespace fs = std::filesystem;

    fs::path dir(dir_path);
    if (!fs::is_directory(dir)) {
        std::string err = "'" + dir_path + "' is not a directory";
        spdlog::error("TreeParser: {}", err);
        return {};
    }

    auto root_file = dir / "root.json";
    if (!fs::exists(root_file)) {
        std::string err = "'" + root_file.string() + "' not found";
        spdlog::error("TreeParser: {}", err);
        return {};
    }

    std::ifstream rf(root_file);
    if (!rf.is_open()) {
        std::string err = "failed to open '" + root_file.string() + "'";
        spdlog::error("TreeParser: {}", err);
        return {};
    }

    nlohmann::json root_j;
    try {
        rf >> root_j;
    } catch (const nlohmann::json::parse_error& e) {
        std::string err = "failed to parse '" + root_file.string() + "': " + e.what();
        spdlog::error("TreeParser: {}", err);
        return {};
    }

    nlohmann::json subtrees_j = nlohmann::json::object();
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".json") continue;
        if (path.filename() == "root.json") continue;

        auto name = path.stem().string();
        std::ifstream sf(path);
        if (!sf.is_open()) {
            std::string err = "failed to open '" + path.string() + "'";
            spdlog::error("TreeParser: {}", err);
            return {};
        }

        try {
            sf >> subtrees_j[name];
        } catch (const nlohmann::json::parse_error& e) {
            std::string err = "failed to parse '" + path.string() + "': " + e.what();
            spdlog::error("TreeParser: {}", err);
            return {};
        }
    }

    nlohmann::json combined;
    combined["root"] = std::move(root_j);
    if (!subtrees_j.empty()) {
        combined["subtrees"] = std::move(subtrees_j);
    }
    return combined.dump();
}

ParseResult TreeParser::Parse(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.contains("root")) {
            std::string err = "JSON must have 'root' field";
            spdlog::error("TreeParser: {}", err);
            return {nullptr, std::move(err)};
        }

        SubtreeRegistry subtrees;
        if (j.contains("subtrees") && j["subtrees"].is_object()) {
            for (auto it = j["subtrees"].begin(); it != j["subtrees"].end(); ++it) {
                subtrees[it.key()] = it.value();
            }
        }

        uint32_t next_id = 1;
        std::set<std::string> resolving;
        std::string parse_error;
        auto node = ParseNode(j["root"], next_id, subtrees, resolving, parse_error);
        if (!node && parse_error.empty()) {
            parse_error = "failed to parse root node";
            spdlog::error("TreeParser: {}", parse_error);
        }
        return {std::move(node), std::move(parse_error)};
    } catch (const nlohmann::json::parse_error& e) {
        std::string err = std::string("JSON parse error: ") + e.what();
        spdlog::error("TreeParser: {}", err);
        return {nullptr, std::move(err)};
    } catch (const std::exception& e) {
        std::string err = std::string("parse error: ") + e.what();
        spdlog::error("TreeParser: {}", err);
        return {nullptr, std::move(err)};
    }
}

static void SetError(std::string& out, std::string msg) {
    spdlog::error("TreeParser: {}", msg);
    out = std::move(msg);
}

std::unique_ptr<Node> TreeParser::ParseNode(const nlohmann::json& j, uint32_t& next_id,
                                            const SubtreeRegistry& subtrees,
                                            std::set<std::string>& resolving,
                                            std::string& error) {
    if (!j.contains("type")) {
        SetError(error, "node missing 'type' field");
        return nullptr;
    }

    std::string type = j["type"].get<std::string>();
    std::string name = j.value("name", type);

    if (type == "Selector" || type == "Sequence" || type == "Parallel"
        || type == "RandomSelector" || type == "RandomSequence") {
        return ParseComposite(j, next_id, subtrees, resolving, error);
    }
    if (type == "Script") {
        return ParseScriptLeaf(j, next_id, error);
    }
    if (type == "Subtree") {
        return ParseSubtree(j, next_id, subtrees, resolving, error);
    }
    if (type == "Repeat") {
        return ParseRepeat(j, next_id, subtrees, resolving, error);
    }
    if (type == "RetryUntilSuccessful") {
        return ParseRetryUntilSuccessful(j, next_id, subtrees, resolving, error);
    }
    if (type == "Wait") {
        return ParseWait(j, next_id, error);
    }

    SetError(error, "unknown node type '" + type + "'");
    return nullptr;
}

std::vector<std::unique_ptr<Node>> TreeParser::ParseChildren(const nlohmann::json& j, uint32_t& next_id,
                                                             const SubtreeRegistry& subtrees,
                                                             std::set<std::string>& resolving,
                                                             std::string& error) {
    std::vector<std::unique_ptr<Node>> children;
    if (j.contains("children") && j["children"].is_array()) {
        for (const auto& child_j : j["children"]) {
            auto child = ParseNode(child_j, next_id, subtrees, resolving, error);
            if (!child) {
                return {};
            }
            children.push_back(std::move(child));
        }
    }
    return children;
}

std::unique_ptr<Node> TreeParser::ParseComposite(const nlohmann::json& j, uint32_t& next_id,
                                                 const SubtreeRegistry& subtrees,
                                                 std::set<std::string>& resolving,
                                                 std::string& error) {
    std::string type = j["type"].get<std::string>();
    std::string name = j.value("name", type);
    uint32_t id = next_id++;

    std::unique_ptr<Node> node;
    if (type == "Selector") {
        node = std::make_unique<Selector>(id, std::move(name));
    } else if (type == "Sequence") {
        node = std::make_unique<Sequence>(id, std::move(name));
    } else if (type == "Parallel") {
        auto success_policy = j.value("success_policy", "RequireAll");
        auto failure_policy = j.value("failure_policy", "RequireOne");
        node = std::make_unique<Parallel>(id, std::move(name),
                                          ParseParallelPolicy(success_policy),
                                          ParseParallelPolicy(failure_policy));
    } else if (type == "RandomSelector") {
        node = std::make_unique<RandomSelector>(id, std::move(name));
    } else if (type == "RandomSequence") {
        node = std::make_unique<RandomSequence>(id, std::move(name));
    }

    auto* composite = static_cast<Composite*>(node.get());
    for (auto& child : ParseChildren(j, next_id, subtrees, resolving, error)) {
        composite->AddChild(std::move(child));
    }

    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get());
    if (j.contains("description")) {
        node->set_description(j["description"].get<std::string>());
    }
    return node;
}

std::unique_ptr<Node> TreeParser::ParseScriptLeaf(const nlohmann::json& j, uint32_t& next_id,
                                                   std::string& error) {
    if (!j.contains("path")) {
        SetError(error, "Script node missing 'path' field");
        return nullptr;
    }

    std::string path = j["path"].get<std::string>();
    std::string name = j.value("name", path);
    uint32_t id = next_id++;

    ScriptNode::ArgsMap args;
    if (j.contains("args") && j["args"].is_object()) {
        for (auto it = j["args"].begin(); it != j["args"].end(); ++it) {
            auto val = ParseLuaValue(it.value());
            if (val) {
                args[it.key()] = std::move(*val);
            } else {
                spdlog::error("TreeParser: Script node 'args' key '{}' has unsupported type", it.key());
            }
        }
    }

    auto node = std::make_unique<ScriptNode>(id, std::move(name), std::move(path), std::move(args));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get());
    if (j.contains("description")) {
        node->set_description(j["description"].get<std::string>());
    }
    return node;
}

std::unique_ptr<Node> TreeParser::ParseSubtree(const nlohmann::json& j, uint32_t& next_id,
                                               const SubtreeRegistry& subtrees,
                                               std::set<std::string>& resolving,
                                               std::string& error) {
    if (!j.contains("subtree")) {
        SetError(error, "Subtree node missing 'subtree' field");
        return nullptr;
    }

    auto subtree_name = j["subtree"].get<std::string>();

    if (resolving.count(subtree_name)) {
        SetError(error, "circular subtree reference '" + subtree_name + "'");
        return nullptr;
    }

    auto it = subtrees.find(subtree_name);
    if (it == subtrees.end()) {
        SetError(error, "unknown subtree '" + subtree_name + "'");
        return nullptr;
    }

    uint32_t subtree_id = next_id++;
    resolving.insert(subtree_name);
    auto subtree_root = ParseNode(it->second, next_id, subtrees, resolving, error);
    resolving.erase(subtree_name);
    if (!subtree_root) {
        SetError(error, "failed to parse subtree '" + subtree_name + "'");
        return nullptr;
    }

    auto name = j.value("name", subtree_name);
    auto node = std::make_unique<SubtreeNode>(subtree_id, std::move(name),
                                                std::move(subtree_name),
                                                std::move(subtree_root));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get());
    if (j.contains("description")) {
        node->set_description(j["description"].get<std::string>());
    }
    return node;
}

std::unique_ptr<Node> TreeParser::ParseRepeat(const nlohmann::json& j, uint32_t& next_id,
                                               const SubtreeRegistry& subtrees,
                                               std::set<std::string>& resolving,
                                               std::string& error) {
    auto children = ParseChildren(j, next_id, subtrees, resolving, error);
    if (children.empty()) {
        SetError(error, "Repeat node requires at least one child");
        return nullptr;
    }

    std::string name = j.value("name", "Repeat");
    uint32_t id = next_id++;
    int count = j.value("count", Repeat::kInfinite);

    auto node = std::make_unique<Repeat>(id, std::move(name), count, std::move(children[0]));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get());
    if (j.contains("description")) {
        node->set_description(j["description"].get<std::string>());
    }
    return node;
}

std::unique_ptr<Node> TreeParser::ParseRetryUntilSuccessful(const nlohmann::json& j, uint32_t& next_id,
                                                            const SubtreeRegistry& subtrees,
                                                            std::set<std::string>& resolving,
                                                            std::string& error) {
    auto children = ParseChildren(j, next_id, subtrees, resolving, error);
    if (children.empty()) {
        SetError(error, "RetryUntilSuccessful node requires at least one child");
        return nullptr;
    }

    std::string name = j.value("name", "RetryUntilSuccessful");
    uint32_t id = next_id++;
    int max_attempts = j.value("attempts", RetryUntilSuccessful::kInfinite);

    auto node = std::make_unique<RetryUntilSuccessful>(id, std::move(name), max_attempts,
                                                        std::move(children[0]));
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get());
    if (j.contains("description")) {
        node->set_description(j["description"].get<std::string>());
    }
    return node;
}

std::unique_ptr<Node> TreeParser::ParseWait(const nlohmann::json& j, uint32_t& next_id,
                                            std::string& /*error*/) {
    std::string name = j.value("name", "Wait");
    uint32_t id = next_id++;
    int ms = j.value("ms", 1000);

    auto node = std::make_unique<WaitNode>(id, std::move(name), ms);
    ApplyDecorators(j, node.get());
    ApplySensors(j, node.get());
    if (j.contains("description")) {
        node->set_description(j["description"].get<std::string>());
    }
    return node;
}

void TreeParser::ApplyDecorators(const nlohmann::json& j, Node* node) {
    if (!j.contains("decorators") || !j["decorators"].is_array()) return;

    for (const auto& dec_j : j["decorators"]) {
        if (!dec_j.contains("type")) continue;

        std::string dec_type = dec_j["type"].get<std::string>();
        auto abort = ParseAbortMode(dec_j.value("abort", "None"));

        if (dec_type == "BlackboardCondition") {
            if (!dec_j.contains("key")) {
                spdlog::error("TreeParser: BlackboardCondition missing 'key'");
                continue;
            }
            auto key = dec_j["key"].get<std::string>();
            auto op = dec_j.value("operator", "is_set");
            std::optional<LuaValue> expected;
            if (dec_j.contains("value")) {
                expected = ParseLuaValue(dec_j["value"]);
            }
            auto dec = std::make_unique<BlackboardCondition>(
                std::move(key), std::move(op), std::move(expected), abort);
            node->AddDecorator(std::move(dec));
        } else if (dec_type == "Inverter") {
            auto dec = std::make_unique<Inverter>(abort);
            if (dec_j.contains("child") && dec_j["child"].is_object()) {
                auto& child_j = dec_j["child"];
                if (child_j.contains("type")) {
                    std::string child_type = child_j["type"].get<std::string>();
                    if (child_type == "BlackboardCondition") {
                        if (child_j.contains("key")) {
                            auto ckey = child_j["key"].get<std::string>();
                            auto cop = child_j.value("operator", "is_set");
                            std::optional<LuaValue> cexpected;
                            if (child_j.contains("value")) {
                                cexpected = ParseLuaValue(child_j["value"]);
                            }
                            dec->set_child(std::make_unique<BlackboardCondition>(
                                std::move(ckey), std::move(cop), std::move(cexpected), abort));
                        }
                    } else if (child_type == "ForceSuccess") {
                        dec->set_child(std::make_unique<ForceSuccess>(abort));
                    } else if (child_type == "ForceFailure") {
                        dec->set_child(std::make_unique<ForceFailure>(abort));
                    }
                }
            }
            node->AddDecorator(std::move(dec));
        } else if (dec_type == "ForceSuccess") {
            auto dec = std::make_unique<ForceSuccess>(abort);
            node->AddDecorator(std::move(dec));
        } else if (dec_type == "ForceFailure") {
            auto dec = std::make_unique<ForceFailure>(abort);
            node->AddDecorator(std::move(dec));
        } else {
            spdlog::warn("TreeParser: unknown decorator type '{}'", dec_type);
        }
    }
}

void TreeParser::ApplySensors(const nlohmann::json& j, Node* node) {
    if (!j.contains("sensors") || !j["sensors"].is_array()) return;

    for (const auto& sen_j : j["sensors"]) {
        if (!sen_j.contains("name") || !sen_j.contains("path")) {
            spdlog::error("TreeParser: sensor missing 'name' or 'path'");
            continue;
        }

        SensorSpec spec;
        spec.name = sen_j["name"].get<std::string>();
        spec.description = sen_j.value("description", "");
        spec.script_path = sen_j["path"].get<std::string>();
        spec.interval_ms = sen_j.value("interval", 100);

        if (sen_j.contains("args") && sen_j["args"].is_object()) {
            for (auto it = sen_j["args"].begin(); it != sen_j["args"].end(); ++it) {
                auto val = ParseLuaValue(it.value());
                if (val) {
                    spec.args[it.key()] = std::move(*val);
                } else {
                    spdlog::error("TreeParser: sensor 'args' key '{}' has unsupported type", it.key());
                }
            }
        }

        node->AddSensorSpec(std::move(spec));
    }
}
