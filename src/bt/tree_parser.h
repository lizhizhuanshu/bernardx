#pragma once

#include <memory>
#include <string>

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
// Path resolution (uniform for root / sensor_defs / Subtree `path`):
//   "@<rel>"  -> project resource: ResourceProvider::Load(<rel>)
//   "<abs>"   -> absolute filesystem path (direct read)
//
//   root_path        — path to a JSON file holding the root node object (required)
//   sensor_defs_path — path to a JSON file {name: {interval, path, params}} (optional)
class TreeParser {
public:
    // bt.ready entry: load root + sensor_defs files, recursively load subtrees by path.
    static async_simple::coro::Lazy<ParseResult> LoadAndParse(
        const std::string& root_path,
        const std::string& sensor_defs_path,
        std::shared_ptr<ResourceProvider> provider);

    // Sync entry (unit tests): parse a root node JSON object directly. No file
    // loading — Subtree nodes are unsupported here (no loader) and will error.
    static ParseResult Parse(const std::string& root_json);
};
