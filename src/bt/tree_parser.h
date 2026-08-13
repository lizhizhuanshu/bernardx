#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include <async_simple/coro/Lazy.h>

class Node;
class ResourceProvider;

// Result of building a behavior-tree Node tree from JSON definitions.
struct ParseResult {
    std::unique_ptr<Node> root;
    std::string error;
};

// Builds a behavior-tree Node tree from JSON definition files.
//
// Path resolution (uniform for root / Subtree `path`):
//   "res://<rel>" -> project resource: ResourceProvider::Load(<rel>)  (rel is the
//                    path under the res/ root, .json included)
//   "<abs>"       -> absolute filesystem path (direct read)
//
//   root_path — path to a JSON file holding the root node object (required)
//
//   params — optional object of template values. When non-empty, every {{key}}
//            placeholder in ANY string field of the root JSON is substituted
//            (same rules as Subtree param forwarding): a whole-value
//            "{{key}}" keeps the param's original type, an embedded placeholder
//            interpolates as text, an unknown key is left literal + warned.
class TreeParser {
public:
    // bt.init entry: load root file, recursively load subtrees by path.
    static async_simple::coro::Lazy<ParseResult> LoadAndParse(
        const std::string& root_path,
        std::shared_ptr<ResourceProvider> provider,
        nlohmann::json params = nlohmann::json::object());

    // Sync entry (unit tests): parse a root node JSON object directly. No file
    // loading — Subtree nodes are unsupported here (no loader) and will error.
    // `params` templating is applied exactly like LoadAndParse's.
    static ParseResult Parse(const std::string& root_json,
                             nlohmann::json params = nlohmann::json::object());
};
