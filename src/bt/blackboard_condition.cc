#include "blackboard_condition.h"

#include <spdlog/spdlog.h>

#include "blackboard.h"

BlackboardCondition::BlackboardCondition(std::string key, std::string op,
                                         std::optional<LuaValue> expected,
                                         AbortMode abort_mode)
    : Decorator("BlackboardCondition", abort_mode),
      key_(std::move(key)),
      op_(std::move(op)),
      expected_(std::move(expected)) {}

bool BlackboardCondition::Evaluate(Blackboard& bb) {
    if (op_ == "is_set") {
        return bb.Has(key_);
    }
    if (op_ == "is_not_set") {
        return !bb.Has(key_);
    }

    auto actual = bb.Get(key_);
    if (!actual.has_value()) {
        return false;
    }

    if (!expected_.has_value()) {
        spdlog::warn("BlackboardCondition: operator '{}' requires expected value", op_);
        return false;
    }

    return EvaluateOp(*actual);
}

bool BlackboardCondition::EvaluateOp(const LuaValue& actual) const {
    return std::visit(
        [&actual, this](const auto& expected) -> bool {
            using ExpT = std::decay_t<decltype(expected)>;
            return std::visit(
                [&expected, this](const auto& act) -> bool {
                    using ActT = std::decay_t<decltype(act)>;
                    if constexpr (std::is_same_v<ExpT, ActT> && !std::is_same_v<ActT, LuaRef>) {
                        if (op_ == "equals") return act == expected;
                        if (op_ == "not_equals") return act != expected;
                        if constexpr (std::is_same_v<ActT, int64_t>) {
                            if (op_ == "greater_than") return act > expected;
                            if (op_ == "less_than") return act < expected;
                            if (op_ == "greater_equal") return act >= expected;
                            if (op_ == "less_equal") return act <= expected;
                        }
                        if constexpr (std::is_same_v<ActT, double>) {
                            if (op_ == "greater_than") return act > expected;
                            if (op_ == "less_than") return act < expected;
                            if (op_ == "greater_equal") return act >= expected;
                            if (op_ == "less_equal") return act <= expected;
                        }
                    }
                    spdlog::warn("BlackboardCondition: type mismatch for operator '{}'", op_);
                    return false;
                },
                actual);
        },
        *expected_);
}
