#include <gtest/gtest.h>

#include <functional>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "blackboard.h"

// --- Dotted-key descent tests ---
//
// A key containing '.' with no literal entry descends into table values:
// Get("proxy.ip") = field ip of blackboard["proxy"]. Tables live as LuaRefs
// into a lua_State; a ref created by LuaRuntime::CreateRef resolves its own
// state, but here we hand-roll a minimal ref (no runtime) that reports a
// directly-held state — same contract the runtime's refs satisfy.

namespace {

class StatefulLuaRef : public LuaRefBase {
public:
    StatefulLuaRef(int r, int t, lua_State* L) : LuaRefBase(r, t), L_(L) {}
    lua_State* state() const override { return L_; }
private:
    lua_State* L_;
};

class DottedKeyTest : public ::testing::Test {
protected:
    void SetUp() override {
        L = luaL_newstate();
    }
    void TearDown() override {
        lua_close(L);
    }

    // Wrap the value at the top of L's stack as an owned LuaValue table ref
    // (pops it), as LuaRuntime::CreateRef would.
    LuaValue WrapTopTable() {
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return LuaValue(std::make_shared<StatefulLuaRef>(ref, LUA_TTABLE, L));
    }

    // Store `{ip = ..., port = ...}` (any fields via fn) under `key`.
    void SetTable(const char* key, const std::function<void()>& fill) {
        lua_newtable(L);
        fill();
        bb.Set(key, WrapTopTable());
    }

    lua_State* L;
    Blackboard bb;
};

}  // namespace

TEST_F(DottedKeyTest, DescendsIntoTable) {
    SetTable("proxy", [this]() {
        lua_pushstring(L, "10.0.0.1");
        lua_setfield(L, -2, "ip");
        lua_pushinteger(L, 8100);
        lua_setfield(L, -2, "port");
    });
    auto ip = bb.Get("proxy.ip");
    ASSERT_TRUE(ip.has_value());
    EXPECT_EQ(std::get<std::string>(*ip), "10.0.0.1");
    auto port = bb.Get("proxy.port");
    ASSERT_TRUE(port.has_value());
    EXPECT_EQ(std::get<int64_t>(*port), 8100);
    EXPECT_FALSE(bb.Get("proxy.host").has_value());  // absent field
}

TEST_F(DottedKeyTest, DescendsMultipleLevels) {
    SetTable("cfg", [this]() {
        lua_newtable(L);                       // cfg.net
        lua_newtable(L);                       // cfg.net.proxy
        lua_pushstring(L, "v1");
        lua_setfield(L, -2, "proto");
        lua_setfield(L, -2, "proxy");          // net.proxy = {...}
        lua_setfield(L, -2, "net");            // cfg.net = {...}
    });
    auto v = bb.Get("cfg.net.proxy.proto");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(std::get<std::string>(*v), "v1");
}

TEST_F(DottedKeyTest, LiteralKeyShadowsDescent) {
    // A literal "proxy.ip" entry wins over descending into "proxy".
    bb.Set("proxy.ip", LuaValue(std::string("literal-wins")));
    SetTable("proxy", [this]() {
        lua_pushstring(L, "from-table");
        lua_setfield(L, -2, "ip");
    });
    EXPECT_EQ(std::get<std::string>(*bb.Get("proxy.ip")), "literal-wins");
    // And the table's other fields are still reachable.
    bb.Remove("proxy.ip");
    EXPECT_EQ(std::get<std::string>(*bb.Get("proxy.ip")), "from-table");
}

TEST_F(DottedKeyTest, MissesYieldNullopt) {
    EXPECT_FALSE(bb.Get("proxy.ip").has_value());          // root missing
    bb.Set("scalar", LuaValue(std::string("s")));
    EXPECT_FALSE(bb.Get("scalar.ip").has_value());         // root not a table
    SetTable("t", [this]() {
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "n");
    });
    EXPECT_FALSE(bb.Get("t.n.deeper").has_value());        // mid-path non-table
    EXPECT_FALSE(bb.Get("t..n").has_value());              // empty segment
    EXPECT_FALSE(bb.Get("t.").has_value());                // trailing dot
}

TEST_F(DottedKeyTest, FlatLookupUnchangedWithoutDot) {
    // Regression guard: no '.' -> exact flat key semantics as before.
    bb.Set("x", LuaValue(static_cast<int64_t>(5)));
    EXPECT_EQ(std::get<int64_t>(*bb.Get("x")), 5);
    EXPECT_FALSE(bb.Get("nope").has_value());
}

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
    bb.SetProvider("n", [&calls](const std::vector<std::string>&) -> LuaValue {
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
    bb.SetProvider("double_base", [&bb](const std::vector<std::string>&) -> LuaValue {
        auto base = bb.Get("base");  // re-enters the blackboard (no lock held)
        return LuaValue(static_cast<int64_t>(std::get<int64_t>(*base) * 2));
    });
    EXPECT_EQ(std::get<int64_t>(*bb.Get("double_base")), 20);
    bb.Set("base", LuaValue(static_cast<int64_t>(50)));
    EXPECT_EQ(std::get<int64_t>(*bb.Get("double_base")), 100);  // live
}

TEST(BlackboardTest, SetReplacesProviderAndViceVersa) {
    Blackboard bb;
    bb.SetProvider("k", [](const std::vector<std::string>&) -> LuaValue { return LuaValue(std::string("computed")); });
    EXPECT_EQ(std::get<std::string>(*bb.Get("k")), "computed");
    bb.Set("k", LuaValue(std::string("static")));
    EXPECT_EQ(std::get<std::string>(*bb.Get("k")), "static");
    bb.SetProvider("k", [](const std::vector<std::string>&) -> LuaValue { return LuaValue(std::string("again")); });
    EXPECT_EQ(std::get<std::string>(*bb.Get("k")), "again");
}

TEST(BlackboardTest, ProviderHasRemoveClear) {
    Blackboard bb;
    bb.SetProvider("p", [](const std::vector<std::string>&) -> LuaValue { return LuaValue(static_cast<int64_t>(1)); });
    EXPECT_TRUE(bb.Has("p"));  // provider counts as present
    bb.Remove("p");
    EXPECT_FALSE(bb.Has("p"));
    bb.SetProvider("q", [](const std::vector<std::string>&) -> LuaValue { return LuaValue(static_cast<int64_t>(1)); });
    bb.Clear();
    EXPECT_FALSE(bb.Has("q"));
}

TEST(BlackboardTest, ProviderReturningNilIsPresentNil) {
    Blackboard bb;
    bb.SetProvider("nils", [](const std::vector<std::string>&) -> LuaValue { return LuaValue(nullptr); });
    auto v = bb.Get("nils");
    ASSERT_TRUE(v.has_value());  // present (provider installed)...
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(*v));  // ...but nil
}

TEST(BlackboardTest, ProviderReceivesDottedPathSegments) {
    // A dotted read on a provider root hands the remaining segments to the
    // provider; its return value IS the result (no descent after it).
    Blackboard bb;
    bb.SetProvider("cfg", [](const std::vector<std::string>& path) -> LuaValue {
        if (path.empty()) return LuaValue(std::string("flat"));
        std::string joined;
        for (size_t i = 0; i < path.size(); ++i) {
            joined += (i ? "/" : "") + path[i];
        }
        return LuaValue(joined);
    });
    // Flat read: no segments.
    EXPECT_EQ(std::get<std::string>(*bb.Get("cfg")), "flat");
    // Dotted reads: segments in order, any depth.
    EXPECT_EQ(std::get<std::string>(*bb.Get("cfg.host")), "host");
    EXPECT_EQ(std::get<std::string>(*bb.Get("cfg.net.ip")), "net/ip");
    EXPECT_EQ(std::get<std::string>(*bb.Get("cfg.a.b.c")), "a/b/c");
}


TEST(BlackboardTest, ProviderSelfReadStopsAtRecursionLimit) {
    Blackboard bb;
    // Provider reading its own key: guarded, returns nullopt instead of
    // recursing forever (the nested Get hits the depth cap).
    bb.SetProvider("loop", [&bb](const std::vector<std::string>&) -> LuaValue {
        auto v = bb.Get("loop");
        return v ? *v : LuaValue(nullptr);
    });
    auto v = bb.Get("loop");
    EXPECT_TRUE(v.has_value());  // outer Get returns the provider's nil
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(*v));
}
