#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class Node;

using SubtreeRegistry = std::unordered_map<std::string, nlohmann::json>;

struct ParseResult {
    std::unique_ptr<Node> root;
    std::string error;
};

class TreeParser {
public:
    static ParseResult Parse(const std::string& json_str);

    static std::string LoadTreeFromDirectory(const std::string& dir_path);

private:
    static std::unique_ptr<Node> ParseNode(const nlohmann::json& j, uint32_t& next_id,
                                           const SubtreeRegistry& subtrees,
                                           std::set<std::string>& resolving,
                                           std::string& error);
    static std::vector<std::unique_ptr<Node>> ParseChildren(const nlohmann::json& j, uint32_t& next_id,
                                                            const SubtreeRegistry& subtrees,
                                                            std::set<std::string>& resolving,
                                                            std::string& error);
    static std::unique_ptr<Node> ParseComposite(const nlohmann::json& j, uint32_t& next_id,
                                                const SubtreeRegistry& subtrees,
                                                std::set<std::string>& resolving,
                                                std::string& error);
    static std::unique_ptr<Node> ParseScriptLeaf(const nlohmann::json& j, uint32_t& next_id,
                                                  std::string& error);
    static std::unique_ptr<Node> ParseSubtree(const nlohmann::json& j, uint32_t& next_id,
                                              const SubtreeRegistry& subtrees,
                                              std::set<std::string>& resolving,
                                              std::string& error);
    static std::unique_ptr<Node> ParseRepeat(const nlohmann::json& j, uint32_t& next_id,
                                             const SubtreeRegistry& subtrees,
                                             std::set<std::string>& resolving,
                                             std::string& error);
    static std::unique_ptr<Node> ParseRetryUntilSuccessful(const nlohmann::json& j, uint32_t& next_id,
                                                           const SubtreeRegistry& subtrees,
                                                           std::set<std::string>& resolving,
                                                           std::string& error);
    static std::unique_ptr<Node> ParseWait(const nlohmann::json& j, uint32_t& next_id,
                                           std::string& error);
    static void ApplyDecorators(const nlohmann::json& j, Node* node);
    static void ApplySensors(const nlohmann::json& j, Node* node);
};
