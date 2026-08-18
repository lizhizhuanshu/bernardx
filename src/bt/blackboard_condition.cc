#include "blackboard_condition.h"

#include <optional>
#include <utility>
#include <variant>

#include "blackboard.h"
#include "lua_runtime.h"
#include "provider_call.h"
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

// The value a completed provider read delivered (nil for an empty return
// or an error — errors are logged inside provider_call).
LuaValue ResultValue(const ScriptResult& r) {
    return (!r.values.empty()) ? r.values[0] : LuaValue(nullptr);
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

BlackboardCondition::~BlackboardCondition() {
    CancelPending();
}

async_simple::coro::Lazy<bool> BlackboardCondition::Init(lua_State* /*L*/,
                                                         LuaRuntime* ctx) {
    lua_ctx_ = ctx;  // backs provider get coroutines
    co_return true;
}

void BlackboardCondition::CancelPending() {
    if (!lua_ctx_) {
        lhs_pend_ = {};
        rhs_pend_ = {};
        return;
    }
    if (lhs_pend_.co != nullptr) {
        lua_ctx_->CancelCall(lhs_pend_.co);
        lhs_pend_ = {};
    }
    if (rhs_pend_.co != nullptr) {
        lua_ctx_->CancelCall(rhs_pend_.co);
        rhs_pend_ = {};
    }
}

bool BlackboardCondition::StartRead(Blackboard& bb, const std::string& key,
                                    std::optional<LuaValue>& out,
                                    PendingCall& pend) {
    auto look = bb.Lookup(key);
    switch (look.kind) {
    case BbReadResult::Kind::kValue:
        out = std::move(look.value);
        return false;
    case BbReadResult::Kind::kMissing:
        return false;  // stays nullopt -> comparison treats as not met
    case BbReadResult::Kind::kProvider: {
        if (!lua_ctx_) {
            spdlog::warn(
                "Blackboard condition: provider key '{}' has no LuaRuntime",
                key);
            return false;
        }
        auto res = provider_call::InvokeProviderGet(
            lua_ctx_, look.provider, look.path,
            [&pend](ScriptResult r) {
                pend.done = true;
                pend.result = std::move(r);
            });
        if (res.yielded) {
            pend.co = res.co;
            return true;  // in flight
        }
        // Synchronous provider completion: the callback already fired.
        out = ResultValue(pend.result);
        pend = {};
        return false;
    }
    }
    return false;
}

NodeStatus BlackboardCondition::Compare() {
    lhs_pend_ = {};
    rhs_pend_ = {};

    if (!lhs_val_.has_value()) {
        // Missing key is "not met" for every comparison op — including "!=".
        return NodeStatus::kFailure;
    }
    LuaValue rhs = rhs_;
    if (!key2_.empty()) {
        if (!rhs_val_.has_value()) {
            return NodeStatus::kFailure;  // rhs missing -> not met
        }
        rhs = *rhs_val_;
    }

    const LuaValue& lhs = *lhs_val_;
    if (op_ == "==") return ValuesEqual(lhs, rhs) ? NodeStatus::kSuccess : NodeStatus::kFailure;
    if (op_ == "!=") return ValuesEqual(lhs, rhs) ? NodeStatus::kFailure : NodeStatus::kSuccess;

    auto ord = ValuesOrder(lhs, rhs);
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

NodeStatus BlackboardCondition::Tick(Blackboard& bb, BtEventQueue& /*events*/) {
    if (op_ == "exists") {
        return bb.Has(key_) ? NodeStatus::kSuccess : NodeStatus::kFailure;
    }

    // Finish in-flight provider reads (both sides run in parallel).
    if (lhs_pend_.co != nullptr || rhs_pend_.co != nullptr) {
        if (lhs_pend_.co != nullptr) {
            if (!lhs_pend_.done) return NodeStatus::kRunning;
            lhs_val_ = ResultValue(lhs_pend_.result);
        }
        if (rhs_pend_.co != nullptr) {
            if (!rhs_pend_.done) return NodeStatus::kRunning;
            rhs_val_ = ResultValue(rhs_pend_.result);
        }
        return Compare();
    }

    // Fresh evaluation: read both sides (provider reads may suspend).
    lhs_val_.reset();
    rhs_val_.reset();
    bool in_flight = StartRead(bb, key_, lhs_val_, lhs_pend_);
    if (!key2_.empty()) {
        in_flight |= StartRead(bb, key2_, rhs_val_, rhs_pend_);
    } else {
        rhs_val_ = rhs_;  // literal right side
    }
    if (in_flight) return NodeStatus::kRunning;
    return Compare();
}

void BlackboardCondition::Reset() {
    CancelPending();
    NodeCondition::Reset();
}
