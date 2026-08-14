#pragma once

#include <string>

#include "bt_utils.h"
#include "leaf.h"
#include "lua_types.h"

// A built-in action that writes the blackboard — no Lua script needed.
// JSON shape: {"type":"Set","params":{"key":"page","value":"home"}}
//   key    (string, required)  blackboard key to write
//   value  (scalar, optional)  string/number/bool; absent writes nil. A
//                              string starting with '$' is a blackboard
//                              reference ('$src' copies blackboard[src] at
//                              each Tick; '$$x' escapes to the literal "$x").
// Tick writes the value and returns Success immediately. '$src' with a
// missing/nil source writes nil with a warning (consistent with `$key`
// params resolution).
class SetNode : public Leaf {
public:
    // Literal form: writes `value` every Tick.
    SetNode(uint32_t id, std::string name, std::string key, LuaValue value);

    // Reference form: writes blackboard[ref_key] (read fresh) every Tick.
    SetNode(uint32_t id, std::string name, std::string key, BbParamRef ref);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;

private:
    std::string key_;
    LuaValue value_ = LuaValue(nullptr);  // literal (ref form ignores)
    std::string ref_key_;                 // non-empty -> write bb.Get(ref_key_)
};
