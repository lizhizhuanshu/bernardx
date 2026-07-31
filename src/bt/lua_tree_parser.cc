#include "lua_tree_parser.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <spdlog/spdlog.h>

#include "blackboard_condition.h"
#include "composite.h"
#include "decorator.h"
#include "force_failure.h"
#include "force_success.h"
#include "inverter.h"
#include "lua_value_utils.h"
#include "node.h"
#include "parallel.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "repeat.h"
#include "retry_until_successful.h"
#include "script_node.h"
#include "selector.h"
#include "sequence.h"
#include "resume_sequence.h"
#include "subtree_node.h"
#include "types.h"
#include "wait_node.h"

namespace {

// Context carried through the recursive parse. All stack indices are absolute.
struct ParseContext {
    int subtrees_idx = 0;
    int sensors_idx = 0;
    uint32_t next_id = 1;
    std::set<std::string> resolving;
    std::string error;
};

using ArgsMap = std::unordered_map<std::string, LuaValue>;

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

// --- Stack field readers (all leave the stack unchanged) ---

std::string ReadStringField(lua_State* L, int tbl, const char* key, const char* def = "") {
    std::string result = def ? def : "";
    lua_getfield(L, tbl, key);
    if (lua_isstring(L, -1)) result = lua_tostring(L, -1);
    lua_pop(L, 1);
    return result;
}

int64_t ReadIntField(lua_State* L, int tbl, const char* key, int64_t def) {
    int64_t result = def;
    lua_getfield(L, tbl, key);
    if (lua_isinteger(L, -1)) {
        result = lua_tointeger(L, -1);
    } else if (lua_isnumber(L, -1)) {
        result = static_cast<int64_t>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    return result;
}

ArgsMap ReadArgsMap(lua_State* L, int tbl, const char* key) {
    ArgsMap args;
    lua_getfield(L, tbl, key);
    if (lua_istable(L, -1)) {
        int args_tbl = lua_absindex(L, -1);
        lua_pushnil(L);
        while (lua_next(L, args_tbl)) {
            // key at -2, value at -1
            if (lua_isstring(L, -2)) {
                std::string k = lua_tostring(L, -2);
                args.emplace(std::move(k), LuaValueFromStack(L, -1));
            }
            lua_pop(L, 1);  // pop value, keep key for next iteration
        }
    }
    lua_pop(L, 1);
    return args;
}

void ReadDescription(lua_State* L, int idx, Node* node) {
    lua_getfield(L, idx, "description");
    if (lua_isstring(L, -1)) node->set_description(lua_tostring(L, -1));
    lua_pop(L, 1);
}

// Forward declarations
std::unique_ptr<Node> ParseNode(lua_State* L, int idx, ParseContext& ctx);
SensorSpec ReadSensorSpec(lua_State* L, int sensors_idx, const std::string& name);

void ApplyDecorators(lua_State* L, int idx, Node* node);
void ApplySensors(lua_State* L, int idx, Node* node, ParseContext& ctx);

std::unique_ptr<Decorator> ParseDecoratorChild(lua_State* L, int idx, AbortMode abort) {
    std::string type = ReadStringField(L, idx, "type", "");
    if (type == "BlackboardCondition") {
        std::string key = ReadStringField(L, idx, "key", "");
        std::string op = ReadStringField(L, idx, "operator", "is_set");
        std::optional<LuaValue> expected;
        lua_getfield(L, idx, "value");
        if (!lua_isnil(L, -1)) expected = LuaValueFromStack(L, -1);
        lua_pop(L, 1);
        if (key.empty()) return nullptr;
        return std::make_unique<BlackboardCondition>(std::move(key), std::move(op),
                                                      std::move(expected), abort);
    }
    if (type == "ForceSuccess") return std::make_unique<ForceSuccess>(abort);
    if (type == "ForceFailure") return std::make_unique<ForceFailure>(abort);
    return nullptr;
}

void ApplyDecorators(lua_State* L, int idx, Node* node) {
    lua_getfield(L, idx, "decorators");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int arr = lua_absindex(L, -1);
    lua_Integer n = luaL_len(L, arr);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, arr, static_cast<int>(i));
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        int dec_idx = lua_absindex(L, -1);
        std::string type = ReadStringField(L, dec_idx, "type", "");
        if (type.empty()) {
            lua_pop(L, 1);
            continue;
        }
        auto abort = ParseAbortMode(ReadStringField(L, dec_idx, "abort", "None"));

        if (type == "BlackboardCondition") {
            std::string key = ReadStringField(L, dec_idx, "key", "");
            if (key.empty()) {
                spdlog::warn("LuaTreeParser: BlackboardCondition missing 'key'");
            } else {
                std::string op = ReadStringField(L, dec_idx, "operator", "is_set");
                std::optional<LuaValue> expected;
                lua_getfield(L, dec_idx, "value");
                if (!lua_isnil(L, -1)) expected = LuaValueFromStack(L, -1);
                lua_pop(L, 1);
                node->AddDecorator(std::make_unique<BlackboardCondition>(
                    std::move(key), std::move(op), std::move(expected), abort));
            }
        } else if (type == "Inverter") {
            auto dec = std::make_unique<Inverter>(abort);
            lua_getfield(L, dec_idx, "child");
            if (lua_istable(L, -1)) {
                int child_idx = lua_absindex(L, -1);
                auto child = ParseDecoratorChild(L, child_idx, abort);
                if (child) dec->set_child(std::move(child));
            }
            lua_pop(L, 1);
            node->AddDecorator(std::move(dec));
        } else if (type == "ForceSuccess") {
            node->AddDecorator(std::make_unique<ForceSuccess>(abort));
        } else if (type == "ForceFailure") {
            node->AddDecorator(std::make_unique<ForceFailure>(abort));
        } else {
            spdlog::warn("LuaTreeParser: unknown decorator type '{}'", type);
        }
        lua_pop(L, 1);  // decorator entry
    }
    lua_pop(L, 1);  // decorators array
}

SensorSpec ReadSensorSpec(lua_State* L, int sensors_idx, const std::string& name) {
    SensorSpec spec;
    spec.name = name;
    spec.interval_ms = 100;
    lua_getfield(L, sensors_idx, name.c_str());
    if (lua_istable(L, -1)) {
        int cfg = lua_absindex(L, -1);
        spec.description = ReadStringField(L, cfg, "description", "");
        spec.interval_ms = ReadIntField(L, cfg, "interval", 100);
        spec.script_path = ReadStringField(L, cfg, "path", "");
        spec.args = ReadArgsMap(L, cfg, "params");
    }
    lua_pop(L, 1);
    return spec;
}

void ApplySensors(lua_State* L, int idx, Node* node, ParseContext& ctx) {
    lua_getfield(L, idx, "sensors");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int arr = lua_absindex(L, -1);
    lua_Integer n = luaL_len(L, arr);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, arr, static_cast<int>(i));
        if (lua_isstring(L, -1)) {
            std::string name = lua_tostring(L, -1);
            lua_pop(L, 1);
            SensorSpec spec = ReadSensorSpec(L, ctx.sensors_idx, name);
            if (spec.script_path.empty()) {
                ctx.error = "sensor '" + name + "' is not defined or missing 'path'";
                lua_pop(L, 1);  // sensors array
                return;
            }
            node->AddSensorSpec(std::move(spec));
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);  // sensors array
}

std::unique_ptr<Node> ParseComposite(lua_State* L, int idx, const std::string& type,
                                     ParseContext& ctx) {
    std::string name = ReadStringField(L, idx, "name", type.c_str());
    uint32_t id = ctx.next_id++;

    std::unique_ptr<Node> node;
    if (type == "Selector") {
        node = std::make_unique<Selector>(id, name);
    } else if (type == "Sequence") {
        node = std::make_unique<Sequence>(id, name);
    } else if (type == "ResumeSequence") {
        node = std::make_unique<ResumeSequence>(id, name);
    } else if (type == "Parallel") {
        auto sp = ReadStringField(L, idx, "success_policy", "RequireAll");
        auto fp = ReadStringField(L, idx, "failure_policy", "RequireOne");
        node = std::make_unique<Parallel>(id, name, ParseParallelPolicy(sp), ParseParallelPolicy(fp));
    } else if (type == "RandomSelector") {
        node = std::make_unique<RandomSelector>(id, name);
    } else if (type == "RandomSequence") {
        node = std::make_unique<RandomSequence>(id, name);
    }
    if (!node) {
        ctx.error = "failed to build composite node '" + type + "'";
        return nullptr;
    }

    lua_getfield(L, idx, "children");
    if (lua_istable(L, -1)) {
        int children_tbl = lua_absindex(L, -1);
        lua_Integer n = luaL_len(L, children_tbl);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, children_tbl, static_cast<int>(i));
            auto child = ParseNode(L, -1, ctx);
            lua_pop(L, 1);
            if (!child) {
                lua_pop(L, 1);  // children table
                return nullptr;
            }
            static_cast<Composite*>(node.get())->AddChild(std::move(child));
        }
    }
    lua_pop(L, 1);  // children table (or nil)

    ApplyDecorators(L, idx, node.get());
    ApplySensors(L, idx, node.get(), ctx);
    ReadDescription(L, idx, node.get());
    return node;
}

std::unique_ptr<Node> ParseScriptLeaf(lua_State* L, int idx, ParseContext& ctx) {
    std::string path = ReadStringField(L, idx, "path", "");
    if (path.empty()) {
        ctx.error = "Script node missing 'path' field";
        return nullptr;
    }
    std::string name = ReadStringField(L, idx, "name", path.c_str());
    auto args = ReadArgsMap(L, idx, "params");
    uint32_t id = ctx.next_id++;

    auto node = std::make_unique<ScriptNode>(id, std::move(name), std::move(path), std::move(args));
    ApplyDecorators(L, idx, node.get());
    ApplySensors(L, idx, node.get(), ctx);
    ReadDescription(L, idx, node.get());
    return node;
}

std::unique_ptr<Node> ParseSubtree(lua_State* L, int idx, ParseContext& ctx) {
    std::string subtree_name = ReadStringField(L, idx, "path", "");
    if (subtree_name.empty()) {
        ctx.error = "Subtree node missing 'path' field";
        return nullptr;
    }
    if (ctx.resolving.count(subtree_name)) {
        ctx.error = "circular subtree reference '" + subtree_name + "'";
        return nullptr;
    }
    if (ctx.subtrees_idx == 0) {
        ctx.error = "unknown subtree '" + subtree_name + "'";
        return nullptr;
    }

    lua_getfield(L, ctx.subtrees_idx, subtree_name.c_str());
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        ctx.error = "unknown subtree '" + subtree_name + "'";
        return nullptr;
    }
    int sub_def = lua_absindex(L, -1);
    ctx.resolving.insert(subtree_name);
    auto sub_root = ParseNode(L, sub_def, ctx);
    ctx.resolving.erase(subtree_name);
    lua_pop(L, 1);

    if (!sub_root) {
        if (ctx.error.empty()) ctx.error = "failed to parse subtree '" + subtree_name + "'";
        return nullptr;
    }

    std::string name = ReadStringField(L, idx, "name", subtree_name.c_str());
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<SubtreeNode>(id, std::move(name), std::move(subtree_name),
                                              std::move(sub_root));
    ApplyDecorators(L, idx, node.get());
    ApplySensors(L, idx, node.get(), ctx);
    ReadDescription(L, idx, node.get());
    return node;
}

// Reads children[1] for wrapper nodes (Repeat / RetryUntilSuccessful).
std::unique_ptr<Node> ParseSingleChildWrapper(lua_State* L, int idx, ParseContext& ctx,
                                              const char* missing_msg) {
    lua_getfield(L, idx, "children");
    std::unique_ptr<Node> child;
    if (lua_istable(L, -1)) {
        int children_tbl = lua_absindex(L, -1);
        lua_rawgeti(L, children_tbl, 1);
        if (lua_istable(L, -1)) {
            child = ParseNode(L, -1, ctx);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (!child && ctx.error.empty()) ctx.error = missing_msg;
    return child;
}

std::unique_ptr<Node> ParseRepeat(lua_State* L, int idx, ParseContext& ctx) {
    auto child = ParseSingleChildWrapper(L, idx, ctx, "Repeat node requires at least one child");
    if (!child) return nullptr;
    int count = static_cast<int>(ReadIntField(L, idx, "count", Repeat::kInfinite));
    std::string name = ReadStringField(L, idx, "name", "Repeat");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<Repeat>(id, std::move(name), count, std::move(child));
    ApplyDecorators(L, idx, node.get());
    ApplySensors(L, idx, node.get(), ctx);
    ReadDescription(L, idx, node.get());
    return node;
}

std::unique_ptr<Node> ParseRetry(lua_State* L, int idx, ParseContext& ctx) {
    auto child = ParseSingleChildWrapper(L, idx, ctx, "RetryUntilSuccessful node requires at least one child");
    if (!child) return nullptr;
    int attempts = static_cast<int>(ReadIntField(L, idx, "attempts", RetryUntilSuccessful::kInfinite));
    std::string name = ReadStringField(L, idx, "name", "RetryUntilSuccessful");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<RetryUntilSuccessful>(id, std::move(name), attempts, std::move(child));
    ApplyDecorators(L, idx, node.get());
    ApplySensors(L, idx, node.get(), ctx);
    ReadDescription(L, idx, node.get());
    return node;
}

std::unique_ptr<Node> ParseWait(lua_State* L, int idx, ParseContext& ctx) {
    int ms = static_cast<int>(ReadIntField(L, idx, "ms", 1000));
    std::string name = ReadStringField(L, idx, "name", "Wait");
    uint32_t id = ctx.next_id++;
    auto node = std::make_unique<WaitNode>(id, std::move(name), ms);
    ApplyDecorators(L, idx, node.get());
    ApplySensors(L, idx, node.get(), ctx);
    ReadDescription(L, idx, node.get());
    return node;
}

std::unique_ptr<Node> ParseNode(lua_State* L, int idx, ParseContext& ctx) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) {
        ctx.error = "node definition must be a table";
        return nullptr;
    }
    std::string type = ReadStringField(L, idx, "type", "");
    if (type.empty()) {
        ctx.error = "node missing 'type' field";
        return nullptr;
    }

    if (type == "Selector" || type == "Sequence" || type == "ResumeSequence" ||
        type == "Parallel" || type == "RandomSelector" || type == "RandomSequence") {
        return ParseComposite(L, idx, type, ctx);
    }
    if (type == "Script") return ParseScriptLeaf(L, idx, ctx);
    if (type == "Subtree") return ParseSubtree(L, idx, ctx);
    if (type == "Repeat") return ParseRepeat(L, idx, ctx);
    if (type == "RetryUntilSuccessful") return ParseRetry(L, idx, ctx);
    if (type == "Wait") return ParseWait(L, idx, ctx);

    ctx.error = "unknown node type '" + type + "'";
    return nullptr;
}

}  // namespace

LuaParseResult LuaTreeParser::Parse(lua_State* L, int tree_idx, int subtrees_idx, int sensors_idx) {
    LuaParseResult result;
    tree_idx = lua_absindex(L, tree_idx);

    if (!lua_istable(L, tree_idx)) {
        result.error = "root must be a table";
        return result;
    }

    ParseContext ctx;
    ctx.subtrees_idx = subtrees_idx ? lua_absindex(L, subtrees_idx) : 0;
    ctx.sensors_idx = sensors_idx ? lua_absindex(L, sensors_idx) : 0;

    auto root = ParseNode(L, tree_idx, ctx);
    if (!root) {
        result.error = ctx.error.empty() ? "failed to parse tree" : ctx.error;
        spdlog::error("LuaTreeParser: {}", result.error);
        return result;
    }
    result.root = std::move(root);
    return result;
}
