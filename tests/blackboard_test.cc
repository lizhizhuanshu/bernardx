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
