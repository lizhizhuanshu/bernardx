#pragma once

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class Node;

using SubtreeRegistry = std::unordered_map<std::string, nlohmann::json>;

class TreeParser {
public:
    static std::unique_ptr<Node> Parse(const std::string& json_str);

    // Load tree from a directory: root.json + other .json files as subtrees
    // Returns combined JSON string, or empty string on error
    static std::string LoadTreeFromDirectory(const std::string& dir_path);

private:
    static std::unique_ptr<Node> ParseNode(const nlohmann::json& j, uint32_t& next_id,
                                           const SubtreeRegistry& subtrees,
                                           std::set<std::string>& resolving);
    static std::vector<std::unique_ptr<Node>> ParseChildren(const nlohmann::json& j, uint32_t& next_id,
                                                            const SubtreeRegistry& subtrees,
                                                            std::set<std::string>& resolving);
    static std::unique_ptr<Node> ParseComposite(const nlohmann::json& j, uint32_t& next_id,
                                                const SubtreeRegistry& subtrees,
                                                std::set<std::string>& resolving);
    static std::unique_ptr<Node> ParseScriptLeaf(const nlohmann::json& j, uint32_t& next_id);
    static std::unique_ptr<Node> ParseSubtree(const nlohmann::json& j, uint32_t& next_id,
                                              const SubtreeRegistry& subtrees,
                                              std::set<std::string>& resolving);
    static std::unique_ptr<Node> ParseRepeat(const nlohmann::json& j, uint32_t& next_id,
                                             const SubtreeRegistry& subtrees,
                                             std::set<std::string>& resolving);
    static std::unique_ptr<Node> ParseRetryUntilSuccessful(const nlohmann::json& j, uint32_t& next_id,
                                                           const SubtreeRegistry& subtrees,
                                                           std::set<std::string>& resolving);
    static std::unique_ptr<Node> ParseWait(const nlohmann::json& j, uint32_t& next_id);
    static void ApplyDecorators(const nlohmann::json& j, Node* node);
    static void ApplySensors(const nlohmann::json& j, Node* node);
};
