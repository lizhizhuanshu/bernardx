#pragma once

#include <optional>
#include <string>

#include "decorator.h"
#include "lua_types.h"

class BlackboardCondition : public Decorator {
public:
    BlackboardCondition(std::string key, std::string op,
                        std::optional<LuaValue> expected = std::nullopt,
                        AbortMode abort_mode = AbortMode::kNone);

    bool Evaluate(Blackboard& bb) override;

private:
    bool EvaluateOp(const LuaValue& actual) const;

    std::string key_;
    std::string op_;
    std::optional<LuaValue> expected_;
};
