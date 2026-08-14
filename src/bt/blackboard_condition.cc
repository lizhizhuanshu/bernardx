#include "blackboard_condition.h"

#include <optional>
#include <variant>

#include "blackboard.h"
#include "lua_types.h"
#include "types.h"

namespace {

// Numeric view of a LuaValue (int64_t and double both promote). nullopt when
// the value is not a number.
std::optional<double> AsNumber(const LuaValue& v) {
    if (const auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    if (const auto* d = std::get_if<double>(&v)) return *d;
    return std::nullopt;
}

// Type-aware equality between two blackboard-side values: numbers compare
// numerically across int/float, strings/bools compare like-for-like, null
// matches nil, anything else (mixed types, table refs) is not equal.
bool ValuesEqual(const LuaValue& a, const LuaValue& b) {
    if (std::holds_alternative<std::nullptr_t>(a) ||
        std::holds_alternative<std::nullptr_t>(b)) {
        return std::holds_alternative<std::nullptr_t>(a) &&
               std::holds_alternative<std::nullptr_t>(b);
    }
    if (const auto* ba = std::get_if<bool>(&a)) {
        const auto* bb = std::get_if<bool>(&b);
        return bb && (*ba == *bb);
    }
    if (const auto* sa = std::get_if<std::string>(&a)) {
        const auto* sb = std::get_if<std::string>(&b);
        return sb && (*sa == *sb);
    }
    auto na = AsNumber(a);
    auto nb = AsNumber(b);
    if (na && nb) return *na == *nb;
    return false;  // mixed / non-scalar types
}

// Three-way ordering between two values: -1 / 0 / +1, or nullopt when the
// pair is not orderable (needs both numeric, or both strings).
std::optional<int> ValuesOrder(const LuaValue& a, const LuaValue& b) {
    auto na = AsNumber(a);
    auto nb = AsNumber(b);
    if (na && nb) return (*na < *nb) ? -1 : ((*na > *nb) ? 1 : 0);
    const auto* sa = std::get_if<std::string>(&a);
    const auto* sb = std::get_if<std::string>(&b);
    if (sa && sb) return (*sa < *sb) ? -1 : ((*sa > *sb) ? 1 : 0);
    return std::nullopt;
}

}  // namespace

// static
LuaValue BlackboardCondition::ScalarJsonToLuaValue(const nlohmann::json& v) {
    if (v.is_null()) return LuaValue(nullptr);
    if (v.is_boolean()) return LuaValue(v.get<bool>());
    if (v.is_number_integer() || v.is_number_unsigned())
        return LuaValue(static_cast<int64_t>(v.get<int64_t>()));
    if (v.is_number_float()) return LuaValue(v.get<double>());
    if (v.is_string()) return LuaValue(v.get_ref<const std::string&>());
    return LuaValue(nullptr);
}

BlackboardCondition::BlackboardCondition(std::string key, std::string op,
                                         nlohmann::json value)
    : NodeCondition("Blackboard"),
      key_(std::move(key)),
      op_(std::move(op)),
      rhs_(ScalarJsonToLuaValue(value)) {}

BlackboardCondition::BlackboardCondition(std::string key, std::string op,
                                         std::string key2)
    : NodeCondition("Blackboard"),
      key_(std::move(key)),
      key2_(std::move(key2)),
      op_(std::move(op)) {}

NodeStatus BlackboardCondition::Tick(Blackboard& bb, BtEventQueue& /*events*/) {
    if (op_ == "exists") {
        return bb.Has(key_) ? NodeStatus::kSuccess : NodeStatus::kFailure;
    }

    auto lhs = bb.Get(key_);
    if (!lhs.has_value()) {
        // Missing key is "not met" for every comparison op — including "!=".
        return NodeStatus::kFailure;
    }

    LuaValue rhs = rhs_;
    if (!key2_.empty()) {
        // Live key-vs-key: read the right side fresh from the blackboard.
        auto r = bb.Get(key2_);
        if (!r.has_value()) return NodeStatus::kFailure;  // rhs missing -> not met
        rhs = *r;
    }

    if (op_ == "==") return ValuesEqual(*lhs, rhs) ? NodeStatus::kSuccess : NodeStatus::kFailure;
    if (op_ == "!=") return ValuesEqual(*lhs, rhs) ? NodeStatus::kFailure : NodeStatus::kSuccess;

    auto ord = ValuesOrder(*lhs, rhs);
    if (!ord) {
        set_last_error("Blackboard condition key '" + key_ +
                       "' value not orderable against op '" + op_ + "'");
        return NodeStatus::kFailure;
    }
    bool met = (op_ == ">")  ? (*ord > 0)
             : (op_ == ">=") ? (*ord >= 0)
             : (op_ == "<")  ? (*ord < 0)
                             : (*ord <= 0);  // "<="
    return met ? NodeStatus::kSuccess : NodeStatus::kFailure;
}
