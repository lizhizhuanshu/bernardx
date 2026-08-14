#include <gtest/gtest.h>

#include "blackboard.h"

// --- Blackboard Tests ---

TEST(BlackboardTest, SetAndGet) {
    Blackboard bb;
    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));
    auto val = bb.Get("hp");
    ASSERT_TRUE(val.has_value());
    auto* hp = std::get_if<int64_t>(&*val);
    ASSERT_NE(hp, nullptr);
    EXPECT_EQ(*hp, 100);
}

TEST(BlackboardTest, GetMissingKey) {
    Blackboard bb;
    EXPECT_FALSE(bb.Get("missing").has_value());
}

TEST(BlackboardTest, HasKey) {
    Blackboard bb;
    EXPECT_FALSE(bb.Has("x"));
    bb.Set("x", LuaValue(std::string("hello")));
    EXPECT_TRUE(bb.Has("x"));
}

TEST(BlackboardTest, RemoveKey) {
    Blackboard bb;
    bb.Set("x", LuaValue(static_cast<int64_t>(42)));
    EXPECT_TRUE(bb.Has("x"));
    bb.Remove("x");
    EXPECT_FALSE(bb.Has("x"));
}

TEST(BlackboardTest, Clear) {
    Blackboard bb;
    bb.Set("a", LuaValue(static_cast<int64_t>(1)));
    bb.Set("b", LuaValue(std::string("two")));
    bb.Clear();
    EXPECT_FALSE(bb.Has("a"));
    EXPECT_FALSE(bb.Has("b"));
}

TEST(BlackboardTest, Overwrite) {
    Blackboard bb;
    bb.Set("x", LuaValue(static_cast<int64_t>(1)));
    bb.Set("x", LuaValue(std::string("updated")));
    auto val = bb.Get("x");
    ASSERT_TRUE(val.has_value());
    auto* s = std::get_if<std::string>(&*val);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "updated");
}

TEST(BlackboardTest, MultipleTypes) {
    Blackboard bb;
    bb.Set("nil_val", LuaValue(nullptr));
    bb.Set("bool_val", LuaValue(true));
    bb.Set("int_val", LuaValue(static_cast<int64_t>(-99)));
    bb.Set("dbl_val", LuaValue(3.14));
    bb.Set("str_val", LuaValue(std::string("text")));

    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(*bb.Get("nil_val")));
    EXPECT_EQ(std::get<bool>(*bb.Get("bool_val")), true);
    EXPECT_EQ(std::get<int64_t>(*bb.Get("int_val")), -99);
    EXPECT_DOUBLE_EQ(std::get<double>(*bb.Get("dbl_val")), 3.14);
    EXPECT_EQ(std::get<std::string>(*bb.Get("str_val")), "text");
}

// --- Value provider Tests ---
//
// SetProvider installs a computed-value source: every Get invokes it fresh.
// A key holds either a static value or a provider — last writer wins.

TEST(BlackboardTest, ProviderInvokedOnEveryGet) {
    Blackboard bb;
    int calls = 0;
    bb.SetProvider("n", [&calls]() -> LuaValue {
        ++calls;
        return LuaValue(static_cast<int64_t>(calls));
    });
    EXPECT_EQ(std::get<int64_t>(*bb.Get("n")), 1);
    EXPECT_EQ(std::get<int64_t>(*bb.Get("n")), 2);
    EXPECT_EQ(std::get<int64_t>(*bb.Get("n")), 3);
}

TEST(BlackboardTest, ProviderCanReadOtherKeys) {
    Blackboard bb;
    bb.Set("base", LuaValue(static_cast<int64_t>(10)));
    bb.SetProvider("double_base", [&bb]() -> LuaValue {
        auto base = bb.Get("base");  // re-enters the blackboard (no lock held)
        return LuaValue(static_cast<int64_t>(std::get<int64_t>(*base) * 2));
    });
    EXPECT_EQ(std::get<int64_t>(*bb.Get("double_base")), 20);
    bb.Set("base", LuaValue(static_cast<int64_t>(50)));
    EXPECT_EQ(std::get<int64_t>(*bb.Get("double_base")), 100);  // live
}

TEST(BlackboardTest, SetReplacesProviderAndViceVersa) {
    Blackboard bb;
    bb.SetProvider("k", []() -> LuaValue { return LuaValue(std::string("computed")); });
    EXPECT_EQ(std::get<std::string>(*bb.Get("k")), "computed");
    bb.Set("k", LuaValue(std::string("static")));
    EXPECT_EQ(std::get<std::string>(*bb.Get("k")), "static");
    bb.SetProvider("k", []() -> LuaValue { return LuaValue(std::string("again")); });
    EXPECT_EQ(std::get<std::string>(*bb.Get("k")), "again");
}

TEST(BlackboardTest, ProviderHasRemoveClear) {
    Blackboard bb;
    bb.SetProvider("p", []() -> LuaValue { return LuaValue(static_cast<int64_t>(1)); });
    EXPECT_TRUE(bb.Has("p"));  // provider counts as present
    bb.Remove("p");
    EXPECT_FALSE(bb.Has("p"));
    bb.SetProvider("q", []() -> LuaValue { return LuaValue(static_cast<int64_t>(1)); });
    bb.Clear();
    EXPECT_FALSE(bb.Has("q"));
}

TEST(BlackboardTest, ProviderReturningNilIsPresentNil) {
    Blackboard bb;
    bb.SetProvider("nils", []() -> LuaValue { return LuaValue(nullptr); });
    auto v = bb.Get("nils");
    ASSERT_TRUE(v.has_value());  // present (provider installed)...
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(*v));  // ...but nil
}

TEST(BlackboardTest, ProviderSelfReadStopsAtRecursionLimit) {
    Blackboard bb;
    // Provider reading its own key: guarded, returns nullopt instead of
    // recursing forever (the nested Get hits the depth cap).
    bb.SetProvider("loop", [&bb]() -> LuaValue {
        auto v = bb.Get("loop");
        return v ? *v : LuaValue(nullptr);
    });
    auto v = bb.Get("loop");
    EXPECT_TRUE(v.has_value());  // outer Get returns the provider's nil
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(*v));
}
