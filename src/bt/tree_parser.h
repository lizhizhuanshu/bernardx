#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include <async_simple/coro/Lazy.h>

class Blackboard;
class Node;
class ResourceProvider;
struct lua_State;

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
//   root_path — path to a JSON file holding the root node object (required);
//            a `.bt` path is accepted too and compiled by the DSL compiler
//            (bt_dsl.h) into the same node JSON before anything else
//
//   params — optional object of template values. When non-empty, every {{key}}
//            placeholder in ANY string field of the root JSON is substituted
//            (same rules as Subtree param forwarding): a whole-value "{{key}}"
//            keeps the param's original type, an embedded placeholder
//            interpolates as text, an unknown key is left literal + warned.
//
//   defaults — optional object of bt.init global ranges for Pipeline edge
//            params: `timeout`, `retry`, `response` (each an int, a [lo,hi]
//            array, or a "$key" string). Applied as each Pipeline's BOTTOM
//            fallback when a step carries no `*param` and `params.<param>` is
//            absent, i.e.: step `*X` > params.X > global default.
//
//   blackboard — optional; when present, Pipeline edge params (`*timeout`,
//            `*retry`) accept a "$key" blackboard reference: the value is read
//            from the blackboard AT PARSE TIME (providers invoke fresh) and
//            must be an integer or a [lo,hi] pair, then behaves exactly like
//            the literal forms. An unresolvable reference is a parse ERROR
//            (fail fast), not a silent 0.
class TreeParser {
public:
    // bt.init entry: load root file, recursively load subtrees by path.
    // `blackboard`/`L` (optional) enable "$key" blackboard references in
    // Pipeline edge params (`*timeout`/`*retry`) — read at parse time; L is
    // the runtime's main state, needed when a referenced value is a table.
    static async_simple::coro::Lazy<ParseResult> LoadAndParse(
        const std::string& root_path,
        std::shared_ptr<ResourceProvider> provider,
        nlohmann::json params = nlohmann::json::object(),
        nlohmann::json defaults = nlohmann::json::object(),
        std::shared_ptr<Blackboard> blackboard = nullptr,
        lua_State* L = nullptr,
        std::string registry_path = {});  // .bt 根的动词注册表（空 = 内嵌默认）

    // Sync entry (unit tests): parse a root node JSON object directly. No file
    // loading — Subtree nodes are unsupported here (no loader) and will error.
    // `params` templating is applied exactly like LoadAndParse's.
    static ParseResult Parse(const std::string& root_json,
                             nlohmann::json params = nlohmann::json::object(),
                             nlohmann::json defaults = nlohmann::json::object(),
                             std::shared_ptr<Blackboard> blackboard = nullptr);
};
