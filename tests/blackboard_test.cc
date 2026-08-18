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

    // A stand-in provider table ref (type LUA_TTABLE). Lookup/Store never
    // invoke the provider — the ref's contents are irrelevant here, only
    // its identity and type matter.
    LuaRef NewProviderTable() {
        lua_newtable(L);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return std::make_shared<StatefulLuaRef>(ref, LUA_TTABLE, L);
    }
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

// --- Dotted-key WRITE tests ---
//
// Set("a.b", v): a literal "a.b" entry wins; a provider root routes to the
// provider's Set; a static table root rawsets into the table; a missing or
// non-table root stores a plain literal key.

TEST_F(DottedKeyTest, DottedWriteRoutesToProviderSet) {
    // A provider root reports kProvider with the remaining segments — the
    // caller (SetNode / bb.set) invokes the provider's set itself. Nothing
    // is stored under a literal key.
    LuaRef tbl = NewProviderTable();
    bb.SetProvider("cfg", tbl);
    auto w = bb.Store("cfg.net.ip", LuaValue(static_cast<int64_t>(42)));
    EXPECT_EQ(w.kind, BbWriteResult::Kind::kProvider);
    EXPECT_EQ(w.path, (std::vector<std::string>{"net", "ip"}));
    EXPECT_EQ(w.provider.get(), tbl.get());  // the installed table ref
    EXPECT_FALSE(bb.Has("cfg.net.ip"));      // no literal entry created
}

TEST_F(DottedKeyTest, DottedWriteRawsetsIntoStaticTable) {
    // The write lands in the very table Lua holds: keep our own registry
    // ref to the same object and read it back Lua-side.
    lua_newtable(L);
    lua_pushvalue(L, -1);
    int own = luaL_ref(L, LUA_REGISTRYINDEX);
    bb.Set("t", WrapTopTable());

    bb.Set("t.x", LuaValue(static_cast<int64_t>(7)));
    EXPECT_EQ(std::get<int64_t>(*bb.Get("t.x")), 7);  // engine read-back

    lua_rawgeti(L, LUA_REGISTRYINDEX, own);  // the SAME table object
    lua_getfield(L, -1, "x");
    EXPECT_EQ(lua_tointeger(L, -1), 7);      // Lua-side visible
    lua_pop(L, 2);
    luaL_unref(L, LUA_REGISTRYINDEX, own);
}

TEST_F(DottedKeyTest, DottedWriteRawsetsNested) {
    SetTable("cfg", [this]() {
        lua_newtable(L);                        // cfg.net
        lua_pushstring(L, "1.1.1.1");
        lua_setfield(L, -2, "ip");
        lua_setfield(L, -2, "net");             // cfg.net = {ip=..}
    });
    bb.Set("cfg.net.ip", LuaValue(std::string("2.2.2.2")));
    EXPECT_EQ(std::get<std::string>(*bb.Get("cfg.net.ip")), "2.2.2.2");
}

TEST_F(DottedKeyTest, DottedWriteMidPathMissIsNoOp) {
    // No auto-vivify: a missing mid field (or a non-table mid value, an
    // empty segment) warns and drops the write — nothing is stored.
    SetTable("t", [this]() {
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "n");
    });
    bb.Set("t.nope.x", LuaValue(static_cast<int64_t>(5)));   // missing mid field
    bb.Set("t.n.deeper", LuaValue(static_cast<int64_t>(5)));  // non-table mid
    bb.Set("t..n", LuaValue(static_cast<int64_t>(5)));        // empty segment
    bb.Set("t.", LuaValue(static_cast<int64_t>(5)));          // trailing dot
    EXPECT_FALSE(bb.Get("t.nope.x").has_value());
    EXPECT_FALSE(bb.Get("t.n.deeper").has_value());
    EXPECT_FALSE(bb.Has("t.nope.x"));  // no literal entries created either
    EXPECT_FALSE(bb.Has("t..n"));
    EXPECT_FALSE(bb.Has("t."));
    EXPECT_FALSE(bb.Has("t.n.deeper"));
}

TEST_F(DottedKeyTest, DottedWriteRootMissingOrScalarFallsBackToLiteral) {
    bb.Set("a.b", LuaValue(std::string("v1")));      // root missing -> literal
    EXPECT_TRUE(bb.Has("a.b"));
    EXPECT_EQ(std::get<std::string>(*bb.Get("a.b")), "v1");

    bb.Set("scalar", LuaValue(std::string("s")));
    bb.Set("scalar.ip", LuaValue(std::string("v2")));  // scalar root -> literal
    EXPECT_TRUE(bb.Has("scalar.ip"));
    EXPECT_EQ(std::get<std::string>(*bb.Get("scalar.ip")), "v2");
}

TEST_F(DottedKeyTest, DottedWriteLiteralKeyShadowsRouting) {
    // A literal "p.q" entry already present: Store overwrites the literal
    // (kStored), and the root provider is never consulted (mirrors Lookup).
    bb.Set("p.q", LuaValue(std::string("first")));
    bb.SetProvider("p", NewProviderTable());
    auto w = bb.Store("p.q", LuaValue(std::string("second")));
    EXPECT_EQ(w.kind, BbWriteResult::Kind::kStored);
    EXPECT_EQ(std::get<std::string>(*bb.Get("p.q")), "second");
}

TEST_F(DottedKeyTest, DottedWriteRejectsCrossStateRefValue) {
    SetTable("t", []() {});
    lua_State* L2 = luaL_newstate();
    lua_newtable(L2);
    int r2 = luaL_ref(L2, LUA_REGISTRYINDEX);
    LuaValue foreign(std::make_shared<StatefulLuaRef>(r2, LUA_TTABLE, L2));
    bb.Set("t.x", std::move(foreign));  // ref from another state -> warn, no-op
    EXPECT_FALSE(bb.Get("t.x").has_value());
    luaL_unref(L2, LUA_REGISTRYINDEX, r2);
    lua_close(L2);
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

// --- Provider (Lookup/Store routing) Tests ---
//
// The blackboard stores a provider as the {get,set} table's LuaRef and
// NEVER invokes it: a provider-served read/write reports kProvider (with
// the table ref + remaining path segments) and the CALLER drives the
// provider (see provider_call.h / blackboard_library.cc). These tests
// cover the routing only — invocation behavior lives in the runtime
// integration tests.

TEST_F(DottedKeyTest, ProviderKeyLookupReportsProvider) {
    LuaRef tbl = NewProviderTable();
    bb.SetProvider("cfg", tbl);

    // Flat read: kProvider, no segments, the installed ref.
    auto flat = bb.Lookup("cfg");
    EXPECT_EQ(flat.kind, BbReadResult::Kind::kProvider);
    EXPECT_TRUE(flat.path.empty());
    EXPECT_EQ(flat.provider.get(), tbl.get());

    // Dotted read: the remaining segments, any depth.
    auto dotted = bb.Lookup("cfg.net.ip");
    EXPECT_EQ(dotted.kind, BbReadResult::Kind::kProvider);
    EXPECT_EQ(dotted.path, (std::vector<std::string>{"net", "ip"}));
    EXPECT_EQ(dotted.provider.get(), tbl.get());

    // Static-only convenience: provider keys read as missing.
    EXPECT_FALSE(bb.Get("cfg").has_value());
    // Provider counts as present.
    EXPECT_TRUE(bb.Has("cfg"));
}

TEST_F(DottedKeyTest, SetReplacesProviderAndViceVersa) {
    bb.SetProvider("k", NewProviderTable());
    bb.Set("k", LuaValue(std::string("static")));  // flat Set replaces
    auto after_set = bb.Lookup("k");
    EXPECT_EQ(after_set.kind, BbReadResult::Kind::kValue);
    EXPECT_EQ(std::get<std::string>(after_set.value), "static");

    bb.SetProvider("k", NewProviderTable());  // SetProvider replaces
    EXPECT_EQ(bb.Lookup("k").kind, BbReadResult::Kind::kProvider);
}

TEST_F(DottedKeyTest, ProviderHasRemoveClear) {
    bb.SetProvider("p", NewProviderTable());
    EXPECT_TRUE(bb.Has("p"));  // provider counts as present
    bb.Remove("p");
    EXPECT_FALSE(bb.Has("p"));
    bb.SetProvider("q", NewProviderTable());
    bb.Clear();
    EXPECT_FALSE(bb.Has("q"));
}

TEST_F(DottedKeyTest, ProviderLiteralKeyShadowsRoot) {
    // A literal dotted entry wins over the root's provider on both sides.
    bb.Set("cfg.net.ip", LuaValue(std::string("literal")));
    bb.SetProvider("cfg", NewProviderTable());
    auto r = bb.Lookup("cfg.net.ip");
    EXPECT_EQ(r.kind, BbReadResult::Kind::kValue);
    EXPECT_EQ(std::get<std::string>(r.value), "literal");
    bb.Remove("cfg.net.ip");
    EXPECT_EQ(bb.Lookup("cfg.net.ip").kind, BbReadResult::Kind::kProvider);
}
