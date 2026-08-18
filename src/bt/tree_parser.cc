#include "tree_parser.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "resource_provider.h"

#include "composite.h"
#include "condition_composite.h"
#include "constant_node.h"
#include "blackboard.h"
#include "blackboard_condition.h"
#include "inverter.h"
#include "lua_types.h"
#include "node.h"
#include "node_condition.h"
#include "parallel.h"
#include "pipeline.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "repeat.h"
#include "retry.h"
#include "script_condition.h"
#include "script_node.h"
#include "selector.h"
#include "sequence.h"
#include "set_node.h"
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

// --- Data references (`.path`) ---
//
// A root JSON may carry a top-level `data` object: a lookup table of named
// values (e.g. per-screen selectors). Any string value anywhere in the tree
// that begins with '.' is treated as a data reference — the remainder is read
// as a dot-separated path into `data` and the whole value is replaced by what
// is found there (type preserved: a looked-up object stays an object). A miss
// is left literal with a warning. This runs AFTER template substitution, so
// `.{{target}}.title` first becomes `.Home.title` (templated), then resolves to
// data["Home"]["title"]. `data` itself is stripped from the tree before
// parsing: it is lookup state, not a node, and removing it prevents accidental
// self-reference during the walk.

// Resolve a dot-separated `path` (e.g. "Home.title") against `data`. Returns a
// pointer to the value found, or nullptr on any miss / empty segment.
const json* LookupDataPath(const json& data, const std::string& path) {
    const json* cur = &data;
    size_t start = 0;
    while (true) {
        size_t dot = path.find('.', start);
        std::string seg = (dot == std::string::npos)
                              ? path.substr(start)
                              : path.substr(start, dot - start);
        if (seg.empty()) return nullptr;
        if (!cur->is_object() || !cur->contains(seg)) return nullptr;
        cur = &(*cur)[seg];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

// Walk `target` and resolve every `.path` string against `data` (type
// preserving on hit, left literal with a warning on miss).
void ResolveDataRefs(json& target, const json& data) {
    if (target.is_object() || target.is_array()) {
        for (auto& el : target) ResolveDataRefs(el, data);
        return;
    }
    if (!target.is_string()) return;
    std::string s = target.get<std::string>();
    if (s.empty() || s[0] != '.') return;
    std::string path = TrimWs(s.substr(1));
    if (path.empty()) return;  // lone '.' — leave literal
    const json* found = LookupDataPath(data, path);
    if (found) {
        target = *found;
    } else {
        spdlog::warn("TreeParser: data reference '{}' not found under 'data'", path);
    }
}

// Extract the root's `data` object (if any), strip it from the tree, then
// resolve `.path` references across the whole tree against it.
void ApplyDataResolution(json& root_j) {
    if (!root_j.contains("data") || !root_j["data"].is_object()) return;
    json data = root_j["data"];
    root_j.erase("data");
    if (!data.empty()) ResolveDataRefs(root_j, data);
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

// Load a JSON asset by path. "res://rel" -> ResourceProvider (the resource lives
// under the project res/ root; <rel> is passed verbatim, .json included); any
// other path is an absolute filesystem read.
async_simple::coro::Lazy<std::optional<std::string>>
LoadAsset(const std::string& path, std::shared_ptr<ResourceProvider> provider) {
    constexpr const char* kResScheme = "res://";
    constexpr size_t kResSchemeLen = 6;
    if (path.rfind(kResScheme, 0) == 0) {  // starts with "res://"
        if (!provider) {
            spdlog::error("TreeParser: no resource provider for 'res://' path '{}'", path);
            co_return std::nullopt;
        }
        co_return co_await provider->Load(path.substr(kResSchemeLen));
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

    if (type == "Blackboard") {
        // {"type":"Blackboard","key":"page","op":"==","value":"home"}   key vs literal
        // {"type":"Blackboard","key":"hp","op":">","key2":"shield"}    key vs key (live)
        // op ∈ {"==","!=",">",">=","<","<=","exists"} (default "=="); value
        // must be a scalar; key2 (string) is mutually exclusive with value
        // and invalid for "exists". Validated here so a bad tree fails at
        // parse, not mid-run.
        if (!j.contains("key") || !j["key"].is_string()) {
            SetError(ctx, "Blackboard condition missing 'key'");
            co_return nullptr;
        }
        static const std::set<std::string> kOps = {
            "==", "!=", ">", ">=", "<", "<=", "exists"};
        std::string op = j.value("op", "==");
        if (!kOps.count(op)) {
            SetError(ctx, "Blackboard condition unknown op '" + op + "'");
            co_return nullptr;
        }
        std::string key2;
        if (j.contains("key2")) {
            if (!j["key2"].is_string()) {
                SetError(ctx, "Blackboard condition 'key2' must be a string");
                co_return nullptr;
            }
            if (j.contains("value")) {
                SetError(ctx, "Blackboard condition: use 'value' or 'key2', not both");
                co_return nullptr;
            }
            if (op == "exists") {
                SetError(ctx, "Blackboard condition 'exists' does not use 'key2'");
                co_return nullptr;
            }
            key2 = j["key2"].get<std::string>();
            co_return std::make_unique<BlackboardCondition>(
                j["key"].get<std::string>(), std::move(op), std::move(key2));
        }
        json value = j.contains("value") ? j["value"] : json(nullptr);
        bool scalar = value.is_null() || value.is_boolean() ||
                      value.is_number() || value.is_string();
        if (!scalar) {
            SetError(ctx, "Blackboard condition 'value' must be a scalar");
            co_return nullptr;
        }
        if (op != "==" && op != "!=" && op != "exists" &&
            !(value.is_number() || value.is_string())) {
            SetError(ctx, "Blackboard condition op '" + op +
                              "' needs a number or string 'value'");
            co_return nullptr;
        }
        co_return std::make_unique<BlackboardCondition>(
            j["key"].get<std::string>(), std::move(op), std::move(value));
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

// Parses a *timeout/*retry edge param: an integer (→ fixed value) or a
// [lo, hi] array (→ inclusive random range the Pipeline resolves per run).
// Negative values clamp to 0. A scalar or single-element array yields {v, v};
// out-of-order pairs are normalized to {min, max}. Returns {0, 0} for absent
// or wrong-typed input (0 = wait forever / no retry).
std::pair<int, int> ParseIntRange(const json& v) {
    auto clamp = [](int x) { return x < 0 ? 0 : x; };
    if (v.is_number_integer()) {
        int x = clamp(v.get<int>());
        return {x, x};
    }
    if (v.is_array() && !v.empty() && v[0].is_number_integer()) {
        int a = clamp(v[0].get<int>());
        if (v.size() >= 2 && v[1].is_number_integer()) {
            int b = clamp(v[1].get<int>());
            return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
        }
        return {a, a};
    }
    return {0, 0};
}

// A Pipeline edge param (`*timeout` / `*retry`) as a LAZY spec: the literal
// int (fixed), the [lo,hi] array (a per-run roll), or a "$key" blackboard
// reference read FRESH when the step enters its wait window each run (so
// between runs the value can change and ranges re-roll). Returns nullopt
// only on a malformed shape (caller fails the parse): int / array / "$key" /
// "$$x"-escape are all valid; anything else errors.
std::optional<Pipeline::EdgeParam> ParseEdgeSpec(const json& v, const char* what) {
    using EP = Pipeline::EdgeParam;
    if (v.is_number_integer()) {
        int x = v.get<int>();
        return EP{EP::Kind::kFixed, x, x, {}};
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        auto cls = ResolveBbParamMarker(s);
        if (auto* ref = std::get_if<BbParamRef>(&cls)) {
            return EP{EP::Kind::kBbRef, 0, 0, ref->key};
        }
        // "$$x" escape / lone "$": not a ref. Fall through to the error below
        // (a literal string is not a valid edge param).
    }
    if (v.is_array() && !v.empty() && v.size() <= 2 && v[0].is_number_integer() &&
        (v.size() == 1 || v[1].is_number_integer())) {
        auto [lo0, hi0] = ParseIntRange(v);  // clamps to >=0, sorts
        if (lo0 == hi0) return EP{EP::Kind::kFixed, lo0, hi0, {}};
        return EP{EP::Kind::kRange, lo0, hi0, {}};
    }
    (void)what;  // caller adds context to the error
    return std::nullopt;
}

// The "unset" marker: the step carries no param of its own, so the
// pipeline-level default (params.timeout/params.retry) applies.
Pipeline::EdgeParam kUnsetEdgeParam() {
    using EP = Pipeline::EdgeParam;
    return EP{EP::Kind::kFixed, -1, -1, {}};
}

// A scalar-or-range param value: an integer → {v, v} (RAW, no clamp — the
// caller decides what -1 means); a 1-2 element integer array → a normalized,
// negatives-clamped-to-0 range. nullopt on any other shape. Used by Wait
// `timeout`, Repeat `count` and Retry `max_count`.
std::optional<std::pair<int, int>> ParseScalarOrRange(const json& v) {
    if (v.is_number_integer()) {
        int x = v.get<int>();
        return std::make_pair(x, x);
    }
    if (v.is_array() && !v.empty() && v.size() <= 2 && v[0].is_number_integer() &&
        (v.size() == 1 || v[1].is_number_integer())) {
        return ParseIntRange(v);  // clamps to >=0, sorts
    }
    return std::nullopt;
}

// Pipeline: skip-completed-steps composite. Each child is a normal node
// (built by ParseNode) carrying three pipeline edge params (the `*` prefix
// marks them as edge params the node itself ignores):
//   `*target`  (condition object) — the step's target state; a step whose
//                                target already holds is SKIPPED. Absent =
//                                the action completing IS the target;
//   `*timeout` (int or [lo,hi] MS) — ms to wait for this step's target after
//                                the action ran (0 / absent = wait forever);
//   `*retry`   (int or [lo,hi])  — max re-runs of this step's action when
//                                its target wait times out.
// The two numeric params may be scalars or two-element arrays (uniformly
// random in the inclusive range; resolved per step per run). A child's plain
// `condition` keeps its usual node-guard meaning and is NOT the target. The
// Pipeline node may itself be guarded, so nesting works.
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
        std::shared_ptr<NodeCondition> target;
        if (child_j.contains("*target")) {
            auto cond = co_await ParseCondition(child_j["*target"], ctx);
            if (!cond) co_return nullptr;
            // The target object may carry `abort` ("Self"/"LowerPriority"/
            // "Both") enabling reactive interruption — mirrored from guard
            // semantics (for a target, BECOMING MET is the event).
            if (child_j["*target"].contains("abort") &&
                child_j["*target"]["abort"].is_string()) {
                cond->set_abort(ParseAbortMode(child_j["*target"]["abort"].get<std::string>()));
            }
            target = std::shared_ptr<NodeCondition>(cond.release());
        }
        // Absent edge params stay "unset" so the pipeline-level defaults
        // (params.timeout/params.retry) can supply them; each resolves
        // lazily when the step enters its wait window, per run.
        Pipeline::EdgeParam timeout = kUnsetEdgeParam();
        if (child_j.contains("*timeout")) {
            auto r = ParseEdgeSpec(child_j["*timeout"], "*timeout");
            if (!r) {
                SetError(ctx, "Pipeline step '*timeout' must be a number, a [lo,hi] array or a $key");
                co_return nullptr;
            }
            timeout = std::move(*r);
        }
        Pipeline::EdgeParam retry = kUnsetEdgeParam();
        if (child_j.contains("*retry")) {
            auto r = ParseEdgeSpec(child_j["*retry"], "*retry");
            if (!r) {
                SetError(ctx, "Pipeline step '*retry' must be a number, a [lo,hi] array or a $key");
                co_return nullptr;
            }
            retry = std::move(*r);
        }
        node->AddStep(std::move(child), std::move(target), std::move(timeout), std::move(retry));
    }

    // Pipeline-level defaults: params.timeout / params.retry apply to steps
    // that don't declare their own. Same spec forms, same lazy resolution.
    if (j.contains("params") && j["params"].is_object()) {
        if (j["params"].contains("timeout")) {
            auto r = ParseEdgeSpec(j["params"]["timeout"], "params.timeout");
            if (!r) {
                SetError(ctx, "Pipeline 'params.timeout' must be a number, a [lo,hi] array or a $key");
                co_return nullptr;
            }
            node->SetDefaultTimeout(std::move(*r));
        }
        if (j["params"].contains("retry")) {
            auto r = ParseEdgeSpec(j["params"]["retry"], "params.retry");
            if (!r) {
                SetError(ctx, "Pipeline 'params.retry' must be a number, a [lo,hi] array or a $key");
                co_return nullptr;
            }
            node->SetDefaultRetry(std::move(*r));
        }
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

    // Resolve the subtree's own `.path` data references against its own
    // top-level `data` object — the same treatment the ROOT gets (templating
    // first, then data resolution, so `.{{target}}` becomes `.home` and then
    // looks up data.home).
    ApplyDataResolution(sub_j);

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
    // A Subtree `condition` is normally a guard object. The special string
    // "child_condition" instead transparently adopts the embedded subtree
    // root's own condition (shared, not copied) — so a parent can gate entry
    // to the subtree on a condition defined inside the subtree file. If the
    // root has no condition, the marker is a no-op (warned).
    if (j.contains("condition")) {
        if (j["condition"].is_string()) {
            if (j["condition"].get<std::string>() == "child_condition") {
                auto child_cond = node->child()->shared_condition();
                if (child_cond) {
                    node->SetCondition(child_cond);
                } else {
                    spdlog::warn("TreeParser: Subtree '{}' marks condition 'child_condition' but its root has no condition",
                                 node->subtree_name());
                }
            } else {
                SetError(ctx, "Subtree condition string must be 'child_condition'");
                co_return nullptr;
            }
        } else if (j["condition"].is_object()) {
            if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
        } else {
            SetError(ctx, "Subtree condition must be an object or 'child_condition'");
            co_return nullptr;
        }
    }
    ReadDescription(j, node.get());
    co_return node;
}

// Template: parse-time in-place expansion. Like Subtree (loads `source` JSON,
// forwards `params` via {{}} substitution + its own data refs), but NO runtime
// node exists: the parsed target node replaces the Template node in the tree.
// The Template's own `condition` (if any) is re-attached to the expanded root,
// so the expansion point keeps its guard. Use it when the wrapper would be
// pure noise (e.g. a parameterized step inline in a Pipeline); use Subtree
// when the wrapper itself is meaningful (shared, named, condition-transparent).
async_simple::coro::Lazy<std::unique_ptr<Node>> ParseTemplate(const json& j, ParseContext& ctx) {
    if (!j.contains("source") || !j["source"].is_string()) {
        SetError(ctx, "Template node missing 'source' field");
        co_return nullptr;
    }
    std::string source = j["source"].get<std::string>();

    if (ctx.resolving.count(source)) {
        SetError(ctx, "circular Template reference '" + source + "'");
        co_return nullptr;
    }
    if (!ctx.provider) {
        SetError(ctx, "template loading unavailable for '" + source + "'");
        co_return nullptr;
    }

    auto content = co_await LoadAsset(source, ctx.provider);
    if (!content) {
        SetError(ctx, "failed to load template '" + source + "'");
        co_return nullptr;
    }
    json sub_j;
    try {
        sub_j = json::parse(*content);
    } catch (const json::parse_error& e) {
        SetError(ctx, std::string("failed to parse template '") + source + "': " + e.what());
        co_return nullptr;
    }

    if (j.contains("params") && j["params"].is_object() && !j["params"].empty()) {
        SubstituteTemplates(sub_j, j["params"]);
    }
    ApplyDataResolution(sub_j);

    ctx.resolving.insert(source);
    auto sub_root = co_await ParseNode(sub_j, ctx);
    ctx.resolving.erase(source);

    if (!sub_root) {
        if (ctx.error.empty()) SetError(ctx, "failed to parse template '" + source + "'");
        co_return nullptr;
    }

    // Re-attach the expansion point's own guard (if declared) onto the
    // expanded root — the Template itself no longer exists to carry it.
    if (!co_await ApplyCondition(j, sub_root.get(), ctx)) co_return nullptr;
    ReadDescription(j, sub_root.get());
    co_return sub_root;  // in-place: the expanded node IS this tree position
}

// Wrapper (single-child decorator) nodes take their sole child from the `child`
// field as a direct object — not a `children` array. Composites still use the
// `children` array; the singular/plural split keeps each honest.
async_simple::coro::Lazy<std::unique_ptr<Node>>
ParseSingleChild(const json& j, ParseContext& ctx, const char* missing_msg) {
    if (j.contains("child") && j["child"].is_object()) {
        co_return co_await ParseNode(j["child"], ctx);
    }
    if (ctx.error.empty()) SetError(ctx, missing_msg);
    co_return nullptr;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseRepeat(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "Repeat node requires a child");
    if (!child) co_return nullptr;
    // `count`: a number (-1/absent = infinite) or a [lo,hi] range rolled
    // once per run.
    int lo = Repeat::kInfinite, hi = Repeat::kInfinite;
    if (j.contains("params") && j["params"].is_object() && j["params"].contains("count")) {
        auto r = ParseScalarOrRange(j["params"]["count"]);
        if (!r) {
            SetError(ctx, "Repeat 'count' must be a number or a [lo,hi] array");
            co_return nullptr;
        }
        std::tie(lo, hi) = *r;
    }
    std::string name = j.value("name", "Repeat");
    uint32_t id = ctx.next_id++;
    std::unique_ptr<Node> node = (lo == hi)
        ? std::make_unique<Repeat>(id, std::move(name), lo, std::move(child))
        : std::make_unique<Repeat>(id, std::move(name), lo, hi, std::move(child));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseRetry(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "Retry node requires a child");
    if (!child) co_return nullptr;
    const bool has_params = j.contains("params") && j["params"].is_object();
    // `max_count`: a number (-1/absent = infinite) or a [lo,hi] range rolled
    // once per run.
    int lo = Retry::kInfinite, hi = Retry::kInfinite;
    if (has_params && j["params"].contains("max_count")) {
        auto r = ParseScalarOrRange(j["params"]["max_count"]);
        if (!r) {
            SetError(ctx, "Retry 'max_count' must be a number or a [lo,hi] array");
            co_return nullptr;
        }
        std::tie(lo, hi) = *r;
    }
    // `interval` (ms): the wait between retry attempts — a number (fixed) or
    // a [lo,hi] range rolled once per run. Absent = retry immediately;
    // negative scalars clamp to 0 (same as Wait's ms `timeout`).
    int iv_lo = 0, iv_hi = 0;
    if (has_params && j["params"].contains("interval")) {
        auto r = ParseScalarOrRange(j["params"]["interval"]);
        if (!r) {
            SetError(ctx, "Retry 'interval' must be a number or a [lo,hi] array");
            co_return nullptr;
        }
        std::tie(iv_lo, iv_hi) = *r;
        if (iv_lo < 0) iv_lo = 0;  // -1 is not infinite here
        if (iv_hi < iv_lo) iv_hi = iv_lo;
    }
    std::string name = j.value("name", "Retry");
    uint32_t id = ctx.next_id++;
    std::unique_ptr<Node> node;
    if (iv_lo > 0 || iv_hi > 0) {
        node = std::make_unique<Retry>(id, std::move(name), lo, iv_lo, iv_hi,
                                       std::move(child));
    } else if (lo == hi) {
        node = std::make_unique<Retry>(id, std::move(name), lo, std::move(child));
    } else {
        node = std::make_unique<Retry>(id, std::move(name), lo, hi, std::move(child));
    }
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

// Set: built-in blackboard-write action. {"type":"Set","params":{"key":k,
// "value":v}} writes blackboard[k]=v at Tick. `value` must be a scalar; a
// string starting with '$' is a blackboard reference ('$src' copies
// blackboard[src] fresh at each Tick, '$$x' escapes to the literal "$x").
// Absent value writes nil.
async_simple::coro::Lazy<std::unique_ptr<Node>> ParseSet(const json& j, ParseContext& ctx) {
    if (!j.contains("params") || !j["params"].is_object() ||
        !j["params"].contains("key") || !j["params"]["key"].is_string()) {
        SetError(ctx, "Set node requires params.key (string)");
        co_return nullptr;
    }
    std::string key = j["params"]["key"].get<std::string>();
    std::string name = j.value("name", "Set");
    uint32_t id = ctx.next_id++;

    json v = j["params"].contains("value") ? j["params"]["value"] : json(nullptr);
    if (v.is_string()) {
        auto cls = ResolveBbParamMarker(v.get_ref<const std::string&>());
        if (auto* ref = std::get_if<BbParamRef>(&cls)) {
            co_return std::make_unique<SetNode>(id, std::move(name), std::move(key),
                                                std::move(*ref));
        }
        if (auto* lit = std::get_if<std::string>(&cls)) {
            co_return std::make_unique<SetNode>(id, std::move(name), std::move(key),
                                                LuaValue(*lit));
        }
    }
    if (!(v.is_null() || v.is_boolean() || v.is_number() || v.is_string())) {
        SetError(ctx, "Set node 'value' must be a scalar");
        co_return nullptr;
    }
    // Scalars only (no lua_State needed at parse time).
    LuaValue lv = LuaValue(nullptr);
    if (v.is_boolean()) lv = LuaValue(v.get<bool>());
    else if (v.is_number_integer() || v.is_number_unsigned())
        lv = LuaValue(static_cast<int64_t>(v.get<int64_t>()));
    else if (v.is_number_float()) lv = LuaValue(v.get<double>());
    else if (v.is_string()) lv = LuaValue(v.get_ref<const std::string&>());
    co_return std::make_unique<SetNode>(id, std::move(name), std::move(key),
                                        std::move(lv));
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseWait(const json& j, ParseContext& ctx) {
    // `timeout` (ms): a number (fixed wait) or a [lo, hi] array (uniformly
    // random in the inclusive range, re-rolled per run). Absent = fixed 1000.
    // 0 succeeds immediately. The legacy min_timeout/max_timeout pair was
    // removed — its presence is a hard error so stale trees fail at parse.
    const json* params = nullptr;
    if (j.contains("params") && j["params"].is_object()) params = &j["params"];
    if (params && (params->contains("min_timeout") || params->contains("max_timeout"))) {
        SetError(ctx, "Wait uses 'timeout' (number or [lo,hi]); min_timeout/max_timeout were removed");
        co_return nullptr;
    }
    int lo_ms = 1000, hi_ms = 1000;
    if (params && params->contains("timeout")) {
        auto r = ParseScalarOrRange((*params)["timeout"]);
        if (!r) {
            SetError(ctx, "Wait 'timeout' must be a number or a [lo,hi] array");
            co_return nullptr;
        }
        std::tie(lo_ms, hi_ms) = *r;
        if (lo_ms < 0) lo_ms = 0;  // scalar negatives clamp; -1 is not infinite here
        if (hi_ms < lo_ms) hi_ms = lo_ms;
    }
    std::string name = j.value("name", "Wait");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<WaitNode>(id, std::move(name), lo_ms, hi_ms);
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

async_simple::coro::Lazy<std::unique_ptr<Node>> ParseInverter(const json& j, ParseContext& ctx) {
    auto child = co_await ParseSingleChild(j, ctx, "Inverter node requires a child");
    if (!child) co_return nullptr;
    std::string name = j.value("name", "Inverter");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<Inverter>(id, std::move(name), std::move(child));
    if (!co_await ApplyCondition(j, node.get(), ctx)) co_return nullptr;
    ReadDescription(j, node.get());
    co_return node;
}

// Success / Failure: constant-result leaves. Useful as a guarded terminal
// branch (e.g. a Selector's "already done → succeed" with a condition + abort).
async_simple::coro::Lazy<std::unique_ptr<Node>> ParseConstant(const json& j, ParseContext& ctx) {
    std::string type = j["type"].get<std::string>();
    std::string name = j.value("name", type);
    uint32_t id = ctx.next_id++;
    std::unique_ptr<Node> node = (type == "Success")
        ? std::unique_ptr<Node>(std::make_unique<SuccessNode>(id, name))
        : std::unique_ptr<Node>(std::make_unique<FailureNode>(id, name));
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
    if (type == "Template") co_return co_await ParseTemplate(j, ctx);
    if (type == "Repeat") co_return co_await ParseRepeat(j, ctx);
    if (type == "Retry") co_return co_await ParseRetry(j, ctx);
    if (type == "Set") co_return co_await ParseSet(j, ctx);
    if (type == "Wait") co_return co_await ParseWait(j, ctx);
    if (type == "Inverter") co_return co_await ParseInverter(j, ctx);
    if (type == "Success" || type == "Failure") co_return co_await ParseConstant(j, ctx);

    SetError(ctx, "unknown node type '" + type + "'");
    co_return nullptr;
}

}  // namespace

async_simple::coro::Lazy<ParseResult>
TreeParser::LoadAndParse(const std::string& root_path,
                         std::shared_ptr<ResourceProvider> provider,
                         nlohmann::json params,
                         std::shared_ptr<Blackboard> blackboard,
                         lua_State* L) {
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

    // Treat the root like a Subtree: substitute {{key}} placeholders across
    // every string field using the caller-supplied params (bt.init params).
    if (params.is_object() && !params.empty()) {
        SubstituteTemplates(root_j, params);
    }

    // Resolve `.path` data references against the root's optional `data`
    // object (after templating, so `.{{key}}.field` resolves end-to-end).
    ApplyDataResolution(root_j);

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

ParseResult TreeParser::Parse(const std::string& root_json, nlohmann::json params,
                              std::shared_ptr<Blackboard> blackboard) {
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

    if (params.is_object() && !params.empty()) {
        SubstituteTemplates(root_j, params);
    }

    // Resolve `.path` data references against the root's optional `data`
    // object (after templating, so `.{{key}}.field` resolves end-to-end).
    ApplyDataResolution(root_j);

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
