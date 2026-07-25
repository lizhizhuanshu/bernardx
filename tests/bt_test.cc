#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <filesystem>
#include <thread>

#include "behavior_tree_engine.h"
#include "blackboard.h"
#include "blackboard_condition.h"
#include "bt_event_queue.h"
#include "bt_library.h"
#include "blackboard_library.h"
#include "composite.h"
#include "decorator.h"
#include "force_success.h"
#include "force_failure.h"
#include "inverter.h"
#include "lua_runtime.h"
#include "node.h"
#include "parallel.h"
#include "script_node.h"
#include "selector.h"
#include "sequence.h"
#include "sensor.h"
#include "subtree_node.h"
#include "repeat.h"
#include "retry_until_successful.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "wait_node.h"
#include "lua_tree_parser.h"
#include "file_system_code_provider.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {
// Parse a Lua table expression (e.g. "{type='Selector',...}") into a Node tree
// using LuaTreeParser, bypassing the JSON-based TreeParser (removed).
// Used by BehaviorTreeEngineTest as a replacement for the deleted engine->Load(json).
std::unique_ptr<Node> ParseLuaTree(const std::string& lua_expr) {
    lua_State* L = luaL_newstate();
    if (luaL_dostring(L, ("return " + lua_expr).c_str()) != LUA_OK || !lua_istable(L, -1)) {
        lua_close(L);
        return nullptr;
    }
    lua_newtable(L);  // subtrees (empty)
    lua_newtable(L);  // sensors (empty)
    auto result = LuaTreeParser::Parse(L, -3, -2, -1);
    lua_close(L);
    return std::move(result.root);
}
}  // namespace

// --- Mock node for testing composites without Lua ---

class MockNode : public Node {
public:
    explicit MockNode(uint32_t id, const std::string& name = "mock",
                     NodeStatus status = NodeStatus::kSuccess)
        : Node(id, "Mock", name), status_(status) {}

    void set_status(NodeStatus s) { status_ = s; }

    NodeStatus Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) override {
        ++tick_count;
        return status_;
    }

    int tick_count = 0;
    bool aborted = false;

    void OnAborted() override {
        aborted = true;
        Node::OnAborted();
    }

    void Reset() override {
        tick_count = 0;
        aborted = false;
        Node::Reset();
    }

private:
    NodeStatus status_;
};

// --- BtEventQueue Tests ---

TEST(BtEventQueueTest, PushAndDrain) {
    BtEventQueue q;
    q.Push({"damage", LuaValue(static_cast<int64_t>(10))});
    q.Push({"heal", LuaValue(std::string("potion"))});

    auto events = q.Drain();
    EXPECT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].name, "damage");
    EXPECT_EQ(events[1].name, "heal");

    // Drain again should be empty
    auto empty = q.Drain();
    EXPECT_TRUE(empty.empty());
}

// --- Selector Tests ---

TEST(SelectorTest, FirstChildSuccess) {
    Blackboard bb;
    BtEventQueue events;
    auto sel = std::make_unique<Selector>(1, "sel");
    sel->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    sel->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(SelectorTest, FallsThroughToSecond) {
    Blackboard bb;
    BtEventQueue events;
    auto sel = std::make_unique<Selector>(1, "sel");
    sel->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    sel->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(SelectorTest, AllFail) {
    Blackboard bb;
    BtEventQueue events;
    auto sel = std::make_unique<Selector>(1, "sel");
    sel->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    sel->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kFailure));

    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kFailure);
}

TEST(SelectorTest, RunningRemembersPosition) {
    Blackboard bb;
    BtEventQueue events;
    auto sel = std::make_unique<Selector>(1, "sel");
    auto* mock_a = new MockNode(2, "a", NodeStatus::kFailure);
    auto* mock_b = new MockNode(3, "b", NodeStatus::kRunning);
    sel->AddChild(std::unique_ptr<MockNode>(mock_a));
    sel->AddChild(std::unique_ptr<MockNode>(mock_b));

    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kRunning);
    EXPECT_TRUE(sel->has_started());

    // Second tick should start from child B, not A
    static_cast<MockNode*>(sel->children()[1].get())->set_status(NodeStatus::kSuccess);
    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_FALSE(sel->has_started());
}

TEST(SelectorTest, ResetClearsState) {
    Blackboard bb;
    BtEventQueue events;
    auto sel = std::make_unique<Selector>(1, "sel");
    sel->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    sel->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kRunning));

    sel->Tick(bb, events);  // Running
    EXPECT_TRUE(sel->has_started());

    sel->Reset();
    EXPECT_FALSE(sel->has_started());
}

// --- Sequence Tests ---

TEST(SequenceTest, AllSuccess) {
    Blackboard bb;
    BtEventQueue events;
    auto seq = std::make_unique<Sequence>(1, "seq");
    seq->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    seq->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(SequenceTest, FirstChildFails) {
    Blackboard bb;
    BtEventQueue events;
    auto seq = std::make_unique<Sequence>(1, "seq");
    seq->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    seq->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kFailure);
}

TEST(SequenceTest, SecondChildFails) {
    Blackboard bb;
    BtEventQueue events;
    auto seq = std::make_unique<Sequence>(1, "seq");
    seq->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    seq->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kFailure));

    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kFailure);
}

TEST(SequenceTest, RunningResumesFromSameChild) {
    Blackboard bb;
    BtEventQueue events;
    auto seq = std::make_unique<Sequence>(1, "seq");
    auto* mock_b = new MockNode(3, "b", NodeStatus::kRunning);
    seq->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    seq->AddChild(std::unique_ptr<MockNode>(mock_b));

    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kRunning);
    EXPECT_TRUE(seq->has_started());
    EXPECT_EQ(seq->current_child_index(), 1u);

    // Next tick: child B succeeds
    static_cast<MockNode*>(seq->children()[1].get())->set_status(NodeStatus::kSuccess);
    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_FALSE(seq->has_started());
}

// --- Parallel Tests ---

TEST(ParallelTest, RequireAllSuccess) {
    Blackboard bb;
    BtEventQueue events;
    auto par = std::make_unique<Parallel>(1, "par",
        Parallel::Policy::kRequireAll, Parallel::Policy::kRequireOne);
    par->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    par->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(par->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(ParallelTest, RequireAllOneRunning) {
    Blackboard bb;
    BtEventQueue events;
    auto par = std::make_unique<Parallel>(1, "par",
        Parallel::Policy::kRequireAll, Parallel::Policy::kRequireOne);
    par->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    par->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kRunning));

    EXPECT_EQ(par->Tick(bb, events), NodeStatus::kRunning);
}

TEST(ParallelTest, RequireOneSuccess) {
    Blackboard bb;
    BtEventQueue events;
    auto par = std::make_unique<Parallel>(1, "par",
        Parallel::Policy::kRequireOne, Parallel::Policy::kRequireAll);
    par->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    par->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kRunning));

    EXPECT_EQ(par->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(ParallelTest, RequireOneAllFail) {
    Blackboard bb;
    BtEventQueue events;
    auto par = std::make_unique<Parallel>(1, "par",
        Parallel::Policy::kRequireOne, Parallel::Policy::kRequireAll);
    par->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    par->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kFailure));

    EXPECT_EQ(par->Tick(bb, events), NodeStatus::kFailure);
}

TEST(ParallelTest, AnyFailureWithRequireOne) {
    Blackboard bb;
    BtEventQueue events;
    auto par = std::make_unique<Parallel>(1, "par",
        Parallel::Policy::kRequireAll, Parallel::Policy::kRequireOne);
    par->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    par->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kFailure));

    EXPECT_EQ(par->Tick(bb, events), NodeStatus::kFailure);
}

// --- Nested Composite Tests ---

TEST(NestedTreeTest, SelectorInSequence) {
    Blackboard bb;
    BtEventQueue events;

    // Sequence: [Selector[fail, success], success]
    auto seq = std::make_unique<Sequence>(1, "seq");
    auto sel = std::make_unique<Selector>(2, "sel");
    sel->AddChild(std::make_unique<MockNode>(3, "s1", NodeStatus::kFailure));
    sel->AddChild(std::make_unique<MockNode>(4, "s2", NodeStatus::kSuccess));
    seq->AddChild(std::move(sel));
    seq->AddChild(std::make_unique<MockNode>(5, "c", NodeStatus::kSuccess));

    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(NestedTreeTest, SequenceFailsInnerSelector) {
    Blackboard bb;
    BtEventQueue events;

    // Sequence: [Selector[fail, fail], success]
    auto seq = std::make_unique<Sequence>(1, "seq");
    auto sel = std::make_unique<Selector>(2, "sel");
    sel->AddChild(std::make_unique<MockNode>(3, "s1", NodeStatus::kFailure));
    sel->AddChild(std::make_unique<MockNode>(4, "s2", NodeStatus::kFailure));
    seq->AddChild(std::move(sel));
    seq->AddChild(std::make_unique<MockNode>(5, "c", NodeStatus::kSuccess));

    // Inner selector fails, so sequence fails at child 0
    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kFailure);
}

// --- Decorator Tests ---

TEST(BlackboardConditionTest, IsSet) {
    Blackboard bb;
    BlackboardCondition cond("hp", "is_set");
    EXPECT_FALSE(cond.Evaluate(bb));

    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));
    EXPECT_TRUE(cond.Evaluate(bb));
}

TEST(BlackboardConditionTest, IsNotSet) {
    Blackboard bb;
    BlackboardCondition cond("hp", "is_not_set");
    EXPECT_TRUE(cond.Evaluate(bb));

    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));
    EXPECT_FALSE(cond.Evaluate(bb));
}

TEST(BlackboardConditionTest, EqualsInt) {
    Blackboard bb;
    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));

    BlackboardCondition cond("hp", "equals", LuaValue(static_cast<int64_t>(100)));
    EXPECT_TRUE(cond.Evaluate(bb));

    BlackboardCondition cond2("hp", "equals", LuaValue(static_cast<int64_t>(50)));
    EXPECT_FALSE(cond2.Evaluate(bb));
}

TEST(BlackboardConditionTest, NotEquals) {
    Blackboard bb;
    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));

    BlackboardCondition cond("hp", "not_equals", LuaValue(static_cast<int64_t>(50)));
    EXPECT_TRUE(cond.Evaluate(bb));

    BlackboardCondition cond2("hp", "not_equals", LuaValue(static_cast<int64_t>(100)));
    EXPECT_FALSE(cond2.Evaluate(bb));
}

TEST(BlackboardConditionTest, GreaterThan) {
    Blackboard bb;
    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));

    BlackboardCondition cond("hp", "greater_than", LuaValue(static_cast<int64_t>(50)));
    EXPECT_TRUE(cond.Evaluate(bb));

    BlackboardCondition cond2("hp", "greater_than", LuaValue(static_cast<int64_t>(100)));
    EXPECT_FALSE(cond2.Evaluate(bb));
}

TEST(BlackboardConditionTest, LessThan) {
    Blackboard bb;
    bb.Set("hp", LuaValue(static_cast<int64_t>(50)));

    BlackboardCondition cond("hp", "less_than", LuaValue(static_cast<int64_t>(100)));
    EXPECT_TRUE(cond.Evaluate(bb));

    BlackboardCondition cond2("hp", "less_than", LuaValue(static_cast<int64_t>(50)));
    EXPECT_FALSE(cond2.Evaluate(bb));
}

TEST(BlackboardConditionTest, EqualsString) {
    Blackboard bb;
    bb.Set("state", LuaValue(std::string("idle")));

    BlackboardCondition cond("state", "equals", LuaValue(std::string("idle")));
    EXPECT_TRUE(cond.Evaluate(bb));

    BlackboardCondition cond2("state", "equals", LuaValue(std::string("combat")));
    EXPECT_FALSE(cond2.Evaluate(bb));
}

TEST(BlackboardConditionTest, EqualsBool) {
    Blackboard bb;
    bb.Set("alive", LuaValue(true));

    BlackboardCondition cond("alive", "equals", LuaValue(true));
    EXPECT_TRUE(cond.Evaluate(bb));

    BlackboardCondition cond2("alive", "equals", LuaValue(false));
    EXPECT_FALSE(cond2.Evaluate(bb));
}

TEST(BlackboardConditionTest, TypeMismatchReturnsFalse) {
    Blackboard bb;
    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));

    // String expected but int stored
    BlackboardCondition cond("hp", "equals", LuaValue(std::string("100")));
    EXPECT_FALSE(cond.Evaluate(bb));
}

TEST(BlackboardConditionTest, MissingKeyWithOperatorReturnsFalse) {
    Blackboard bb;
    BlackboardCondition cond("missing", "equals", LuaValue(static_cast<int64_t>(0)));
    EXPECT_FALSE(cond.Evaluate(bb));
}

TEST(InverterTest, InvertsTrue) {
    Blackboard bb;
    bb.Set("alive", LuaValue(true));

    Inverter inv;
    auto child = std::make_unique<BlackboardCondition>("alive", "is_set");
    inv.set_child(std::move(child));
    EXPECT_FALSE(inv.Evaluate(bb));
}

TEST(InverterTest, InvertsFalse) {
    Blackboard bb;
    // "alive" not set, so is_set returns false, inverter returns true
    Inverter inv;
    auto child = std::make_unique<BlackboardCondition>("alive", "is_set");
    inv.set_child(std::move(child));
    EXPECT_TRUE(inv.Evaluate(bb));
}

TEST(ForceSuccessTest, AlwaysTrue) {
    Blackboard bb;
    ForceSuccess fs;
    EXPECT_TRUE(fs.Evaluate(bb));
}

// --- BehaviorTreeEngine Tests ---

class BehaviorTreeEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_shared<BehaviorTreeEngine>();
    }

    void TearDown() override {
        engine->Stop();
    }

    BehaviorTreeEngine::Ptr engine;
};

TEST_F(BehaviorTreeEngineTest, LoadValidTree) {
    engine->SetRoot(ParseLuaTree(
        "{type='Selector',children={{type='Script',path='a.lua'},{type='Script',path='b.lua'}}}"));
    EXPECT_TRUE(engine->IsLoaded());
}

TEST_F(BehaviorTreeEngineTest, StatusBeforeLoad) {
    EXPECT_EQ(engine->GetStatus(), "idle");
}

TEST_F(BehaviorTreeEngineTest, TickOnceOnLoadedTree) {
    engine->SetRoot(ParseLuaTree(
        "{type='Selector',children={{type='Script',path='nonexistent.lua'}}}"));
    EXPECT_TRUE(engine->IsLoaded());

    auto status = engine->TickOnce();
    EXPECT_EQ(status, NodeStatus::kFailure);
}

TEST_F(BehaviorTreeEngineTest, StopResetsTree) {
    engine->SetRoot(ParseLuaTree(
        "{type='Selector',children={{type='Script',path='nonexistent.lua'}}}"));
    engine->Stop();
    EXPECT_EQ(engine->GetStatus(), "idle");
}

TEST_F(BehaviorTreeEngineTest, StopWithoutLoad) {
    engine->Stop();
    EXPECT_EQ(engine->GetStatus(), "idle");
}

TEST_F(BehaviorTreeEngineTest, BlackboardPersistsAcrossLoad) {
    engine->blackboard().Set("x", LuaValue(static_cast<int64_t>(42)));
    EXPECT_TRUE(engine->blackboard().Has("x"));

    engine->SetRoot(ParseLuaTree(
        "{type='Selector',children={{type='Script',path='a.lua'}}}"));
    // SetRoot preserves blackboard state
    EXPECT_TRUE(engine->blackboard().Has("x"));
}

TEST_F(BehaviorTreeEngineTest, EventQueue) {
    engine->Notify("test_event", LuaValue(std::string("data")));
    // Notify shouldn't crash, event is stored internally
    // We can't directly drain event_queue_ since it's private,
    // but we verify Notify doesn't crash
    engine->Notify("another_event", LuaValue(static_cast<int64_t>(42)));
}

// --- BehaviorTreeLibrary Tests ---

class BehaviorTreeLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard = std::make_shared<Blackboard>();
        bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
        lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
        rt = LuaRuntime::Builder()
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    LuaRuntime::Ptr rt;
};

#define AWAIT_BT(lazy) async_simple::coro::syncAwait(lazy)

TEST_F(BehaviorTreeLibraryTest, RequireReturnsTable) {
    auto r = AWAIT_BT(rt->RunScript("local bt = require('bt'); return type(bt)"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "table");
}

TEST_F(BehaviorTreeLibraryTest, HasAllFunctions) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        return type(bt.ready) == 'function'
            and type(bt.exec) == 'function'
            and type(bt.goto_path) == 'function'
            and type(bt.stop) == 'function'
            and type(bt.await) == 'function'
            and type(bt.notify) == 'function'
            and type(bt.get_status) == 'function'
            and type(bt.dump_paths) == 'function'
            and type(bt.path_report) == 'function'
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(BehaviorTreeLibraryTest, GetStatusInitially) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        return bt.get_status()
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "idle");
}

TEST_F(BehaviorTreeLibraryTest, RunInvalidJson) {
    // New API has no JSON string; missing the required `tree` table yields an error.
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({})
        return status, err or 'nil'
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("tree"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, RunInvalidJsonReturnsSpecificError) {
    // A Script node with no path is rejected during parsing.
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({tree = {type = 'Script'}})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("path"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, RunUnknownNodeType) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({tree = {type = 'UnknownType'}})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("UnknownType"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, SetAndGetBlackboard) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bb = require('blackboard')
        bb.set("hp", 100)
        bb.set("name", "hero")
        bb.set("alive", true)
        return bb.get("hp"), bb.get("name"), bb.get("alive"), bb.get("missing")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 100);
    EXPECT_EQ(std::get<std::string>(r.values[1]), "hero");
    EXPECT_EQ(std::get<bool>(r.values[2]), true);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[3]));
}

TEST_F(BehaviorTreeLibraryTest, GetBlackboardAsTable) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bb = require('blackboard')
        bb.set("a", 1)
        bb.set("b", "hello")
        local t = bb.to_table()
        return t.a, t.b
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 1);
    EXPECT_EQ(std::get<std::string>(r.values[1]), "hello");
}

TEST_F(BehaviorTreeLibraryTest, RunLoadAndTick) {
    // x.lua does not exist → script init fails → bt.ready returns nil + error.
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({
            tree = {type = 'Selector', children = {{type = 'Script', path = 'x.lua'}}}
        })
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_FALSE(err->empty());
}

TEST_F(BehaviorTreeLibraryTest, RunWithoutPathOrJson) {
    // No `tree` field → error mentions "tree".
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("tree"), std::string::npos);
}

// --- Abort Mechanism Tests ---

TEST(AbortTest, CollectRunningNodesFromSequence) {
    Blackboard bb;
    BtEventQueue events;

    auto seq = std::make_unique<Sequence>(1, "seq");
    auto* mock_b = new MockNode(3, "b", NodeStatus::kRunning);
    seq->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    seq->AddChild(std::unique_ptr<MockNode>(mock_b));

    // Tick once: child A succeeds, child B runs
    seq->Tick(bb, events);
    EXPECT_TRUE(seq->has_started());

    // Simulate abort
    auto* b_node = dynamic_cast<MockNode*>(seq->children()[1].get());
    EXPECT_FALSE(b_node->aborted);
    seq->OnAborted();
    EXPECT_TRUE(b_node->aborted);
}

TEST(AbortTest, OnAbortedPropagatesToChildren) {
    Blackboard bb;
    BtEventQueue events;

    auto seq = std::make_unique<Sequence>(1, "seq");
    auto* mock_a = new MockNode(2, "a");
    auto* mock_b = new MockNode(3, "b");
    seq->AddChild(std::unique_ptr<MockNode>(mock_a));
    seq->AddChild(std::unique_ptr<MockNode>(mock_b));

    // Tick to advance to child B
    mock_a->set_status(NodeStatus::kSuccess);
    mock_b->set_status(NodeStatus::kRunning);
    seq->Tick(bb, events);

    // Reset and abort
    seq->Reset();
    EXPECT_FALSE(mock_a->aborted);
    EXPECT_FALSE(mock_b->aborted);

    // Tick again and abort
    seq->Tick(bb, events);
    seq->OnAborted();
    EXPECT_TRUE(mock_a->aborted);
    EXPECT_TRUE(mock_b->aborted);
}

// --- Parent/Child relationship Tests ---

TEST(NodeTreeTest, ParentPointersSet) {
    auto seq = std::make_unique<Sequence>(1, "seq");
    auto* child = new MockNode(2, "child");
    seq->AddChild(std::unique_ptr<MockNode>(child));

    EXPECT_EQ(child->parent(), seq.get());
}

TEST(NodeTreeTest, NestedParentPointers) {
    auto root = std::make_unique<Selector>(1, "root");
    auto seq = std::make_unique<Sequence>(2, "seq");
    auto* leaf = new MockNode(3, "leaf");
    seq->AddChild(std::unique_ptr<MockNode>(leaf));
    root->AddChild(std::move(seq));

    EXPECT_EQ(leaf->parent()->parent(), root.get());
}

// --- Decorator on node Tests ---

TEST(DecoratorOnNodeTest, AddDecorator) {
    MockNode node(1, "test");
    EXPECT_TRUE(node.decorators().empty());

    auto cond = std::make_unique<BlackboardCondition>("hp", "is_set");
    node.AddDecorator(std::move(cond));
    EXPECT_EQ(node.decorators().size(), 1u);
}

TEST(DecoratorOnNodeTest, EngineManagesDecoratorState) {
    auto engine = std::make_shared<BehaviorTreeEngine>();
    engine->SetRoot(ParseLuaTree(
        "{type='Selector',"
        "decorators={{type='BlackboardCondition',key='hp',operator='is_set'}},"
        "children={{type='Script',path='a.lua'}}}"));
    EXPECT_TRUE(engine->IsLoaded());
}

// --- BtEventQueue thread safety stress test ---

TEST(BtEventQueueTest, ConcurrentPushDrain) {
    BtEventQueue q;
    constexpr int count = 1000;

    std::thread producer([&] {
        for (int i = 0; i < count; ++i) {
            q.Push({"event_" + std::to_string(i), LuaValue(static_cast<int64_t>(i))});
        }
    });

    std::thread consumer([&] {
        int total = 0;
        while (total < count) {
            auto batch = q.Drain();
            total += static_cast<int>(batch.size());
        }
        EXPECT_EQ(total, count);
    });

    producer.join();
     consumer.join();
}

// --- ScriptNode Colon-Method Integration Tests ---

class ScriptNodeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard = std::make_shared<Blackboard>();
        bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
        lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
        tests_dir_ = std::filesystem::absolute(
            std::filesystem::path(__FILE__).parent_path()).string();
        rt = LuaRuntime::Builder()
            .WithCodeProvider(std::make_shared<FileSystemCodeProvider>(
                std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts", tests_dir_ + "/sensors"}))
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    std::string RunBtScript(const std::string& lua_code) {
        auto r = AWAIT_BT(rt->RunScript(lua_code));
        EXPECT_EQ(r.status, LUA_OK);
        if (auto* b = std::get_if<bool>(&r.values[0])) {
            if (*b && r.values.size() > 1) {
                auto* s = std::get_if<std::string>(&r.values[1]);
                return s ? *s : "success";
            }
            return *b ? "success" : "failure";
        }
        return std::get<std::string>(r.values[0]);
    }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    LuaRuntime::Ptr rt;
    std::string tests_dir_;
};

TEST_F(ScriptNodeIntegrationTest, SelfStateInEnterAndTick) {
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/bt_module.lua'}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ExitReasonAsParameter) {
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/check_reason.lua'}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ArgsPassedToEnter) {
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/with_args.lua', args = {target = 'enemy', damage = 100}}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ArgsBoolType) {
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/bool_args.lua', args = {enabled = true}}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, SelfPersistsAcrossTicks) {
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/counter.lua'}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, NoArgsStillWorks) {
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/no_args.lua'}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ScriptNotFoundReturnsError) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({
            tree = {type = 'Script', path = 'scripts/nonexistent.lua'}
        })
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("nonexistent.lua"), std::string::npos);
}

TEST_F(ScriptNodeIntegrationTest, ScriptRuntimeErrorReturnsError) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/runtime_error.lua'}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    auto* status = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(*status, "failure");
}

TEST_F(ScriptNodeIntegrationTest, NodeReturnsFailureIsNotError) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/returns_failure.lua'}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(ScriptNodeIntegrationTest, InitErrorInSequenceStopsEarly) {
    auto r = AWAIT_BT(rt->RunScript(
        "local bt = require('bt')\n"
        "local status, err = bt.ready({\n"
        "    tree = {type = 'Sequence', children = {\n"
        "        {type = 'Script', path = 'scripts/nonexistent.lua'},\n"
        "        {type = 'Script', path = 'scripts/no_args.lua'},\n"
        "    }}\n"
        "})\n"
        "return status, err\n"
    ));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("nonexistent.lua"), std::string::npos);
}

// --- bt lifecycle exec() max_step / timeout / interval Tests ---

class BtRunOptionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard = std::make_shared<Blackboard>();
        bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
        lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
        tests_dir_ = std::filesystem::absolute(
            std::filesystem::path(__FILE__).parent_path()).string();
        rt = LuaRuntime::Builder()
            .WithCodeProvider(std::make_shared<FileSystemCodeProvider>(
                std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts", tests_dir_ + "/sensors"}))
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    LuaRuntime::Ptr rt;
    std::string tests_dir_;
};

TEST_F(BtRunOptionsTest, MaxStepStopsTree) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/run_forever.lua'}})
        bt.exec({max_step = 2, interval = 10})
        local status, err = bt.await()
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "timeout");
}

TEST_F(BtRunOptionsTest, MaxStepNotReachedTreeCompletes) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/run_3_ticks.lua'}})
        bt.exec({max_step = 100, interval = 10})
        local status, err = bt.await()
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(BtRunOptionsTest, TimeoutStopsTree) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/run_forever.lua'}})
        bt.exec({timeout = 1, interval = 10})
        local status, err = bt.await()
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "timeout");
}

TEST_F(BtRunOptionsTest, TimeoutNotReachedTreeCompletes) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/run_3_ticks.lua'}})
        bt.exec({timeout = 60000, interval = 10})
        local status, err = bt.await()
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(BtRunOptionsTest, IntervalAccepted) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/run_3_ticks.lua'}})
        bt.exec({interval = 10})
        local status, err = bt.await()
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(BtRunOptionsTest, CombinedMaxStepAndInterval) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree = {type = 'Script', path = 'scripts/run_forever.lua'}})
        bt.exec({max_step = 3, interval = 10})
        local status, err = bt.await()
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "timeout");
}

// --- ForceFailure Tests ---

TEST(ForceFailureTest, AlwaysFalse) {
    Blackboard bb;
    ForceFailure ff;
    EXPECT_FALSE(ff.Evaluate(bb));
}

// --- Repeat Tests ---

TEST(RepeatTest, FiniteRepeat) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    auto repeat = std::make_unique<Repeat>(1, "rep", 3,
        std::unique_ptr<MockNode>(child));

    // Tick 1: child succeeds, count=1
    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kRunning);
    // Tick 2: child succeeds, count=2
    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kRunning);
    // Tick 3: child succeeds, count=3 == max
    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RepeatTest, RepeatStopsOnChildFailure) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    auto repeat = std::make_unique<Repeat>(1, "rep", 5,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kRunning);
    child->set_status(NodeStatus::kFailure);
    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kFailure);
}

TEST(RepeatTest, InfiniteRepeatStopsOnFailure) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    auto repeat = std::make_unique<Repeat>(1, "rep", Repeat::kInfinite,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kRunning);
    child->set_status(NodeStatus::kFailure);
    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kFailure);
}

TEST(RepeatTest, ResetClearsState) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    auto repeat = std::make_unique<Repeat>(1, "rep", 2,
        std::unique_ptr<MockNode>(child));

    repeat->Tick(bb, events);  // count=1
    repeat->Reset();
    // After reset, should restart from count=0
    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kRunning);
    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RepeatTest, RunningChildReturnsRunning) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kRunning);
    auto repeat = std::make_unique<Repeat>(1, "rep", 2,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kRunning);
}

TEST(RepeatTest, AbortPropagatesToChild) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kRunning);
    auto repeat = std::make_unique<Repeat>(1, "rep", 2,
        std::unique_ptr<MockNode>(child));

    repeat->Tick(bb, events);
    repeat->OnAborted();
    EXPECT_TRUE(child->aborted);
}

// --- RetryUntilSuccessful Tests ---

TEST(RetryUntilSuccessfulTest, SucceedsImmediately) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RetryUntilSuccessfulTest, RetriesOnFailure) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    // Fail 1
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    // Fail 2
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    // Fail 3: exceeded max attempts
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kFailure);
}

TEST(RetryUntilSuccessfulTest, SucceedsAfterRetries) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    child->set_status(NodeStatus::kSuccess);
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RetryUntilSuccessfulTest, InfiniteRetryNeverGivesUp) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry",
        RetryUntilSuccessful::kInfinite,
        std::unique_ptr<MockNode>(child));

    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    }
    child->set_status(NodeStatus::kSuccess);
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RetryUntilSuccessfulTest, RunningChildReturnsRunning) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kRunning);
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
}

TEST(RetryUntilSuccessfulTest, ResetClearsAttempts) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 2,
        std::unique_ptr<MockNode>(child));

    retry->Tick(bb, events);  // attempt 1
    retry->Reset();
    // After reset, get 2 fresh attempts
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kFailure);
}

// --- RandomSelector Tests ---

TEST(RandomSelectorTest, AllFail) {
    Blackboard bb;
    BtEventQueue events;
    auto rs = std::make_unique<RandomSelector>(1, "rs");
    rs->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    rs->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kFailure));

    EXPECT_EQ(rs->Tick(bb, events), NodeStatus::kFailure);
}

TEST(RandomSelectorTest, OneSucceeds) {
    Blackboard bb;
    BtEventQueue events;
    auto rs = std::make_unique<RandomSelector>(1, "rs");
    rs->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    rs->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(rs->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RandomSelectorTest, RunningRemembered) {
    Blackboard bb;
    BtEventQueue events;
    auto rs = std::make_unique<RandomSelector>(1, "rs");
    rs->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    rs->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kRunning));

    // First child fails, second is running
    EXPECT_EQ(rs->Tick(bb, events), NodeStatus::kRunning);
}

// --- RandomSequence Tests ---

TEST(RandomSequenceTest, AllSucceed) {
    Blackboard bb;
    BtEventQueue events;
    auto rs = std::make_unique<RandomSequence>(1, "rs");
    rs->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    rs->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(rs->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RandomSequenceTest, OneFails) {
    Blackboard bb;
    BtEventQueue events;
    auto rs = std::make_unique<RandomSequence>(1, "rs");
    rs->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kFailure));
    rs->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kSuccess));

    EXPECT_EQ(rs->Tick(bb, events), NodeStatus::kFailure);
}

TEST(RandomSequenceTest, RunningRemembered) {
    Blackboard bb;
    BtEventQueue events;
    auto rs = std::make_unique<RandomSequence>(1, "rs");
    rs->AddChild(std::make_unique<MockNode>(2, "a", NodeStatus::kSuccess));
    rs->AddChild(std::make_unique<MockNode>(3, "b", NodeStatus::kRunning));

    EXPECT_EQ(rs->Tick(bb, events), NodeStatus::kRunning);
}

// --- WaitNode Tests ---

TEST(WaitNodeTest, ZeroMsSucceedsImmediately) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 0);
    EXPECT_EQ(wait->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(WaitNodeTest, ReturnsRunningBeforeTimeout) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 1000);

    // First tick starts the timer
    EXPECT_EQ(wait->Tick(bb, events), NodeStatus::kRunning);
    // Second tick: not enough time has passed
    EXPECT_EQ(wait->Tick(bb, events), NodeStatus::kRunning);
}

TEST(WaitNodeTest, CompletesAfterMs) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 50);

    EXPECT_EQ(wait->Tick(bb, events), NodeStatus::kRunning);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(wait->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(WaitNodeTest, ResetRestartsTimer) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 50);

    wait->Tick(bb, events);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // Timer expired but haven't ticked yet
    wait->Reset();

    // After reset, timer starts fresh
    EXPECT_EQ(wait->Tick(bb, events), NodeStatus::kRunning);
}

// --- TreeParser new node type tests (removed: JSON-based TreeParser deleted) ---

// --- Decorator gating on child nodes ---

TEST(SelectorDecoratorTest, SkipsChildWhenDecoratorFails) {
    Blackboard bb;
    BtEventQueue events;

    auto sel = std::make_unique<Selector>(1, "sel");
    auto* gated = new MockNode(2, "gated", NodeStatus::kSuccess);
    gated->AddDecorator(std::make_unique<BlackboardCondition>("flag", "is_set"));
    auto* fallback = new MockNode(3, "fallback", NodeStatus::kSuccess);

    sel->AddChild(std::unique_ptr<MockNode>(gated));
    sel->AddChild(std::unique_ptr<MockNode>(fallback));

    // flag not set → first child's decorator fails → skip to fallback
    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(gated->tick_count, 0);
    EXPECT_EQ(fallback->tick_count, 1);
}

TEST(SelectorDecoratorTest, TicksChildWhenDecoratorPasses) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("flag", LuaValue(true));

    auto sel = std::make_unique<Selector>(1, "sel");
    auto* gated = new MockNode(2, "gated", NodeStatus::kSuccess);
    gated->AddDecorator(std::make_unique<BlackboardCondition>("flag", "equals", LuaValue(true)));
    auto* fallback = new MockNode(3, "fallback", NodeStatus::kSuccess);

    sel->AddChild(std::unique_ptr<MockNode>(gated));
    sel->AddChild(std::unique_ptr<MockNode>(fallback));

    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(gated->tick_count, 1);
    EXPECT_EQ(fallback->tick_count, 0);
}

TEST(SelectorDecoratorTest, AllDecoratorsFailReturnsFailure) {
    Blackboard bb;
    BtEventQueue events;

    auto sel = std::make_unique<Selector>(1, "sel");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    a->AddDecorator(std::make_unique<BlackboardCondition>("x", "is_set"));
    auto* b = new MockNode(3, "b", NodeStatus::kSuccess);
    b->AddDecorator(std::make_unique<BlackboardCondition>("y", "is_set"));

    sel->AddChild(std::unique_ptr<MockNode>(a));
    sel->AddChild(std::unique_ptr<MockNode>(b));

    EXPECT_EQ(sel->Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(a->tick_count, 0);
    EXPECT_EQ(b->tick_count, 0);
}

TEST(SequenceDecoratorTest, FailsImmediatelyWhenChildDecoratorFails) {
    Blackboard bb;
    BtEventQueue events;

    auto seq = std::make_unique<Sequence>(1, "seq");
    auto* gated = new MockNode(2, "gated", NodeStatus::kSuccess);
    gated->AddDecorator(std::make_unique<BlackboardCondition>("flag", "is_set"));
    auto* second = new MockNode(3, "second", NodeStatus::kSuccess);

    seq->AddChild(std::unique_ptr<MockNode>(gated));
    seq->AddChild(std::unique_ptr<MockNode>(second));

    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(gated->tick_count, 0);
    EXPECT_EQ(second->tick_count, 0);
}

TEST(SequenceDecoratorTest, PassesThroughWhenDecoratorPasses) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("flag", LuaValue(true));

    auto seq = std::make_unique<Sequence>(1, "seq");
    auto* gated = new MockNode(2, "gated", NodeStatus::kSuccess);
    gated->AddDecorator(std::make_unique<BlackboardCondition>("flag", "is_set"));
    auto* second = new MockNode(3, "second", NodeStatus::kSuccess);

    seq->AddChild(std::unique_ptr<MockNode>(gated));
    seq->AddChild(std::unique_ptr<MockNode>(second));

    EXPECT_EQ(seq->Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(gated->tick_count, 1);
    EXPECT_EQ(second->tick_count, 1);
}

TEST(ParallelDecoratorTest, DecoratorFailCountsAsFailure) {
    Blackboard bb;
    BtEventQueue events;

    auto par = std::make_unique<Parallel>(1, "par",
        Parallel::Policy::kRequireAll, Parallel::Policy::kRequireOne);

    auto* gated = new MockNode(2, "gated", NodeStatus::kSuccess);
    gated->AddDecorator(std::make_unique<BlackboardCondition>("flag", "is_set"));
    auto* ok = new MockNode(3, "ok", NodeStatus::kSuccess);

    par->AddChild(std::unique_ptr<MockNode>(gated));
    par->AddChild(std::unique_ptr<MockNode>(ok));

    // gated skipped (decorator fail → failure_count=1), ok succeeds.
    // With failure_policy=RequireOne, one failure → Parallel fails.
    EXPECT_EQ(par->Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(gated->tick_count, 0);
    EXPECT_EQ(ok->tick_count, 1);
}

TEST(RetryDecoratorTest, RetriesWhenChildDecoratorFails) {
    Blackboard bb;
    BtEventQueue events;

    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    child->AddDecorator(std::make_unique<BlackboardCondition>("flag", "is_set"));
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    // Tick 1: decorator fails, attempt=1, returns Running
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    EXPECT_EQ(child->tick_count, 0);

    // Tick 2: decorator still fails, attempt=2
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);

    // Tick 3: decorator still fails, attempt=3 == max → Failure
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(child->tick_count, 0);
}

TEST(RetryDecoratorTest, SucceedsWhenDecoratorStartsPassing) {
    Blackboard bb;
    BtEventQueue events;

    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    child->AddDecorator(std::make_unique<BlackboardCondition>("flag", "is_set"));
    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 5,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);

    bb.Set("flag", LuaValue(true));
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(child->tick_count, 1);
}

TEST(RepeatDecoratorTest, FailsWhenChildDecoratorFails) {
    Blackboard bb;
    BtEventQueue events;

    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    child->AddDecorator(std::make_unique<BlackboardCondition>("flag", "is_set"));
    auto repeat = std::make_unique<Repeat>(1, "rep", 3,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(repeat->Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(child->tick_count, 0);
}

TEST(SubtreeDecoratorTest, BlocksSubtreeWhenDecoratorFails) {
    Blackboard bb;
    BtEventQueue events;

    auto inner = std::make_unique<MockNode>(2, "inner", NodeStatus::kSuccess);
    auto sub = std::make_unique<SubtreeNode>(1, "sub", "test",
        std::move(inner));
    sub->subtree_root()->AddDecorator(
        std::make_unique<BlackboardCondition>("flag", "is_set"));

    EXPECT_EQ(sub->Tick(bb, events), NodeStatus::kFailure);
    auto* inner_node = dynamic_cast<MockNode*>(sub->subtree_root());
    ASSERT_NE(inner_node, nullptr);
    EXPECT_EQ(inner_node->tick_count, 0);
}

TEST(SubtreeDecoratorTest, ExecutesSubtreeWhenDecoratorPasses) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("flag", LuaValue(true));

    auto inner = std::make_unique<MockNode>(2, "inner", NodeStatus::kSuccess);
    auto sub = std::make_unique<SubtreeNode>(1, "sub", "test",
        std::move(inner));
    sub->subtree_root()->AddDecorator(
        std::make_unique<BlackboardCondition>("flag", "is_set"));

    EXPECT_EQ(sub->Tick(bb, events), NodeStatus::kSuccess);
    auto* inner_node = dynamic_cast<MockNode*>(sub->subtree_root());
    ASSERT_NE(inner_node, nullptr);
    EXPECT_EQ(inner_node->tick_count, 1);
}

TEST(DecoratorGateIntegrationTest, RetryWithSelectorGatedScript) {
    Blackboard bb;
    BtEventQueue events;

    auto sel = std::make_unique<Selector>(2, "sel");

    auto* print_script = new MockNode(3, "print", NodeStatus::kSuccess);
    print_script->AddDecorator(std::make_unique<BlackboardCondition>(
        "stop_visible", "equals", LuaValue(true)));

    auto* fail_script = new MockNode(4, "fail", NodeStatus::kFailure);

    sel->AddChild(std::unique_ptr<MockNode>(print_script));
    sel->AddChild(std::unique_ptr<MockNode>(fail_script));

    auto retry = std::make_unique<RetryUntilSuccessful>(1, "retry", 20000,
        std::move(sel));

    // stop_visible not set → first child's decorator fails → skip to fail_script → kFailure → retry
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    // print_script was never ticked (decorator blocked it)
    EXPECT_EQ(print_script->tick_count, 0);

    // Set stop_visible → first child's decorator passes → print_script executes → kSuccess
    bb.Set("stop_visible", LuaValue(true));
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(print_script->tick_count, 1);
}

// --- Non-root Decorator Abort Propagation Tests ---

class AbortPropagationTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard = std::make_shared<Blackboard>();
        bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
        lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
        tests_dir_ = std::filesystem::absolute(
            std::filesystem::path(__FILE__).parent_path()).string();
        rt = LuaRuntime::Builder()
            .WithCodeProvider(std::make_shared<FileSystemCodeProvider>(
                std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts", tests_dir_ + "/sensors"}))
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    LuaRuntime::Ptr rt;
    std::string tests_dir_;
};

#define AWAIT_ABORT(lazy) async_simple::coro::syncAwait(lazy)

TEST_F(AbortPropagationTest, SelfAbortTerminatesTree) {
    // Script runs then clears "go" after 2 ticks. abort=Self on the Script.
    // When "go" is cleared, the decorator transitions true→false, triggering Self abort.
    // Sequence then fails (no more children).
    auto r = AWAIT_ABORT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("go", true)
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script',
                        path = 'scripts/run_then_clear_flag.lua',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'go', operator = 'equals', value = true, abort = 'Self'}
                        }
                    }
                }
            }
        })
        bt.exec({max_step = 20, interval = 10})
        local status = bt.await()
        return status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    // Should be "failure" (aborted) not "timeout" (would happen without abort propagation)
    EXPECT_EQ(*s, "failure");
}

TEST_F(AbortPropagationTest, LowerPriorityAbortPreemptsSibling) {
    // Selector: [no_args (decorator: flag is_set, abort=LowerPriority), run_then_set_flag]
    // run_then_set_flag sets "flag" after 2 ticks.
    // When flag appears, LowerPriority abort fires → second child aborted → first child retries → succeeds.
    auto r = AWAIT_ABORT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.ready({
            tree = {
                type = 'Selector',
                children = {
                    {
                        type = 'Script',
                        path = 'scripts/no_args.lua',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'LowerPriority'}
                        }
                    },
                    {
                        type = 'Script',
                        path = 'scripts/run_then_set_flag.lua'
                    }
                }
            }
        })
        bt.exec({max_step = 20, interval = 10})
        local status = bt.await()
        return status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    // LowerPriority abort should cause Selector to re-evaluate: first child's decorator
    // now passes (flag set) → no_args succeeds → Selector succeeds
    EXPECT_EQ(*s, "success");
}

// --- PathTracer Tests ---

TEST(PathTracerTest, SinglePathCount) {
    BehaviorTreeEngine engine;
    auto leaf = std::make_unique<MockNode>(2, "leaf", NodeStatus::kRunning);
    auto seq = std::make_unique<Sequence>(1, "seq");
    seq->AddChild(std::move(leaf));
    engine.SetRoot(std::move(seq));
    for (int i = 0; i < 5; ++i) engine.TickOnce();

    auto& tr = engine.path_tracer();
    EXPECT_EQ(tr.tick_count(), 5u);
    EXPECT_EQ(tr.path_count(), 1u);
    EXPECT_EQ(tr.count_for({1, 2}), 5u);
    EXPECT_EQ(tr.visits(1), 5);
    EXPECT_EQ(tr.visits(2), 5);
}

TEST(PathTracerTest, RepeatLeafStatusIndependent) {
    // Repeat wraps a success leaf; the leaf records success each tick, but
    // Repeat rewrites its own return to running. Path stats must show BOTH.
    BehaviorTreeEngine engine;
    auto leaf = std::make_unique<MockNode>(2, "leaf", NodeStatus::kSuccess);
    auto rep = std::make_unique<Repeat>(1, "rep", -1, std::move(leaf));
    engine.SetRoot(std::move(rep));
    for (int i = 0; i < 3; ++i) engine.TickOnce();

    auto& tr = engine.path_tracer();
    EXPECT_EQ(tr.count_for({1, 2}), 3u);
    EXPECT_EQ(tr.leaf_status_count({1, 2}, NodeStatus::kSuccess), 3);
    EXPECT_EQ(tr.root_status_count({1, 2}, NodeStatus::kRunning), 3);
    EXPECT_EQ(tr.root_status_count({1, 2}, NodeStatus::kSuccess), 0);
}

TEST(PathTracerTest, ParallelFansOutMultiplePaths) {
    BehaviorTreeEngine engine;
    auto a = std::make_unique<MockNode>(2, "a", NodeStatus::kRunning);
    auto b = std::make_unique<MockNode>(3, "b", NodeStatus::kRunning);
    auto par = std::make_unique<Parallel>(1, "par");
    par->AddChild(std::move(a));
    par->AddChild(std::move(b));
    engine.SetRoot(std::move(par));
    for (int i = 0; i < 4; ++i) engine.TickOnce();

    auto& tr = engine.path_tracer();
    EXPECT_EQ(tr.tick_count(), 4u);
    EXPECT_EQ(tr.path_count(), 2u);
    EXPECT_EQ(tr.count_for({1, 2}), 4u);
    EXPECT_EQ(tr.count_for({1, 3}), 4u);
    EXPECT_EQ(tr.path_occurrences(), 8u);  // 2 paths × 4 ticks
    EXPECT_EQ(tr.visits(1), 4);            // Parallel deduped per tick
    EXPECT_EQ(tr.visits(2), 4);
    EXPECT_EQ(tr.visits(3), 4);
}

TEST(PathTracerTest, ResetBetweenRuns) {
    BehaviorTreeEngine engine;
    engine.SetRoot(std::make_unique<MockNode>(1, "a", NodeStatus::kRunning));
    engine.TickOnce();
    EXPECT_EQ(engine.path_tracer().count_for({1}), 1u);
    // New run: parser ids restart at 1 — data must not leak across runs.
    engine.SetRoot(std::make_unique<MockNode>(1, "b", NodeStatus::kRunning));
    engine.TickOnce();
    auto& tr = engine.path_tracer();
    EXPECT_EQ(tr.path_count(), 1u);
    EXPECT_EQ(tr.count_for({1}), 1u);
}

TEST(PathTracerTest, RootDecoratorBlockedStillSampled) {
    BehaviorTreeEngine engine;
    auto leaf = std::make_unique<MockNode>(2, "leaf", NodeStatus::kSuccess);
    auto root = std::make_unique<Sequence>(1, "root");
    root->AddChild(std::move(leaf));
    root->AddDecorator(std::make_unique<BlackboardCondition>("gate", "is_set"));
    engine.SetRoot(std::move(root));
    engine.TickOnce();  // gate absent → root blocked

    auto& tr = engine.path_tracer();
    EXPECT_EQ(tr.tick_count(), 1u);
    EXPECT_EQ(tr.count_for({1, 2}), 1u);  // path still sampled
}

TEST(PathTracerTest, DecoratorFlipRecorded) {
    BehaviorTreeEngine engine;
    auto leaf = std::make_unique<MockNode>(2, "leaf", NodeStatus::kSuccess);
    auto root = std::make_unique<Sequence>(1, "root");
    root->AddChild(std::move(leaf));
    root->AddDecorator(std::make_unique<BlackboardCondition>("gate", "is_set"));
    engine.SetRoot(std::move(root));
    engine.TickOnce();  // gate absent, dec first seen false (no flip)
    EXPECT_EQ(engine.path_tracer().dec_flip_count(), 0u);
    engine.blackboard().Set("gate", LuaValue(static_cast<int64_t>(1)));
    engine.TickOnce();  // gate set → dec flips false→true
    EXPECT_EQ(engine.path_tracer().dec_flip_count(), 1u);
}

TEST_F(BehaviorTreeLibraryTest, DumpPathsAfterRun) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Wait', ms=999999}})
        bt.exec({max_step=3, interval=1})
        bt.await()
        local p = bt.dump_paths()
        return p.total_ticks, p.path_occurrences, #p.paths, p.terminal, p.has_terminal
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 5u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 3);              // total_ticks
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 3);              // path_occurrences (no Parallel)
    EXPECT_EQ(std::get<int64_t>(r.values[2]), 1);              // 1 path (Wait)
    EXPECT_EQ(std::get<std::string>(r.values[3]), "running");  // terminal
    EXPECT_EQ(std::get<bool>(r.values[4]), true);              // has_terminal (timeout)
}

TEST_F(BehaviorTreeLibraryTest, TracePathsFalseSuppressesCollection) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Wait', ms=999999}, trace_paths=false})
        bt.exec({max_step=3, interval=1})
        bt.await()
        local p = bt.dump_paths()
        return p.total_ticks, p.tracing
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 0);  // not collected
    EXPECT_EQ(std::get<bool>(r.values[1]), false);
}

TEST_F(BehaviorTreeLibraryTest, PathReportReturnsString) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Wait', ms=999999}})
        bt.exec({max_step=2, interval=1})
        bt.await()
        local report = bt.path_report()
        return type(report), #report > 0, string.find(report, '路径报告') ~= nil
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 3u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "string");
    EXPECT_EQ(std::get<bool>(r.values[1]), true);
    EXPECT_EQ(std::get<bool>(r.values[2]), true);  // contains report header
}

// --- lifecycle API: goto_path / exec / await / pause ---

TEST_F(BehaviorTreeLibraryTest, GotoPathLegal) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Sequence', name='root', children={
            {type='Wait', name='w1', ms=99999},
            {type='Wait', name='w2', ms=99999}}}})
        local ok, err = bt.goto_path({'root', 'w2'})
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_GE(r.values.size(), 1u);
    EXPECT_EQ(std::get<bool>(r.values[0]), true);
}

TEST_F(BehaviorTreeLibraryTest, GotoPathNameNotFound) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Sequence', name='root', children={
            {type='Wait', name='w1', ms=99999}}}})
        local ok, err = bt.goto_path({'root', 'nope'})
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    EXPECT_NE(std::get<std::string>(r.values[1]).find("no child"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, GotoPathRejectsParallel) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Parallel', name='par', children={
            {type='Wait', name='w1', ms=99999}}}})
        local ok, err = bt.goto_path({'par', 'w1'})
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    EXPECT_NE(std::get<std::string>(r.values[1]).find("Parallel"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, GotoPathIllegalWhileRunning) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Sequence', name='root', children={
            {type='Wait', name='w1', ms=99999},
            {type='Wait', name='w2', ms=99999}}}})
        bt.exec({interval=10, max_step=100})
        local ok, err = bt.goto_path({'root', 'w2'})  -- running → reject
        bt.stop()
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    EXPECT_NE(std::get<std::string>(r.values[1]).find("running"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, ExecWhileRunningIsNoOp) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Wait', ms=99999}})
        bt.exec({interval=10, max_step=5})
        local s = bt.exec({interval=10})  -- already running → no-op
        bt.await()
        return s
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "running");
}

TEST_F(BehaviorTreeLibraryTest, AwaitAfterDoneReturnsImmediately) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({tree={type='Wait', ms=99999}})
        bt.exec({interval=10, max_step=2})
        local s1 = bt.await()           -- waits for timeout
        local s2 = bt.await()           -- already done → returns outcome immediately
        return s1, s2
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "timeout");
    EXPECT_EQ(std::get<std::string>(r.values[1]), "timeout");
}

TEST_F(BehaviorTreeLibraryTest, RuntimePauseResumeFlag) {
    EXPECT_FALSE(rt->paused());
    rt->Pause();
    EXPECT_TRUE(rt->paused());
    rt->Resume();
    EXPECT_FALSE(rt->paused());
}
