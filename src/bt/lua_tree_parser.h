#pragma once

#include <memory>
#include <string>

extern "C" {
#include "lua.h"
}

class Node;

// Result of parsing a behavior tree from Lua tables on the stack.
struct LuaParseResult {
    std::unique_ptr<Node> root;
    std::string error;
};

// Builds a behavior-tree Node tree directly from Lua tables on the stack,
// bypassing any JSON representation.
//
// tree_idx     — stack position of the `tree` table (root node definition, required)
// subtrees_idx — stack position of the `subtrees` table (name -> node definition)
// sensors_idx  — stack position of the `sensors` table (name -> {interval, args, path})
//
// All indices are normalized to absolute positions internally, so callers may
// pass relative indices. The three tables must remain on the stack for the
// duration of the call.
class LuaTreeParser {
public:
    static LuaParseResult Parse(lua_State* L, int tree_idx, int subtrees_idx, int sensors_idx);
};
