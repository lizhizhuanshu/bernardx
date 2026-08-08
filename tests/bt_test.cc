#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <thread>

#include "behavior_tree_engine.h"
#include "blackboard.h"
#include "bt_event_queue.h"
#include "bt_library.h"
#include "bt_utils.h"
#include "blackboard_library.h"
#include "composite.h"
#include "condition_composite.h"
#include "force_success.h"
#include "force_failure.h"
#include "inverter.h"
#include "lua_runtime.h"
#include "node.h"
#include "node_condition.h"
#include "parallel.h"
#include "pipeline.h"
#include "script_condition.h"
#include "script_node.h"
#include "selector.h"
#include "sequence.h"
#include "subtree_node.h"
#include "repeat.h"
#include "retry_until_successful.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "wait_node.h"
#include "tree_parser.h"
#include "memory_resource_provider.h"
#include "file_system_code_provider.h"

namespace {
// Parse a root node JSON object (e.g. {"type":"Selector",...}) into a Node tree.
// Used by BehaviorTreeEngineTest for sync node-tree construction (no file loading).
std::unique_ptr<Node> ParseJsonTree(const std::string& json) {
    return std::move(TreeParser::Parse(json).root);
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

// MockNode whose accumulated tick count survives Reset() — used to count
// action re-runs across Pipeline retry back-ups (each back-up Reset()s the
// child, clearing MockNode::tick_count).
class CountingMockNode : public MockNode {
public:
    using MockNode::MockNode;
    NodeStatus Tick(Blackboard& bb, BtEventQueue& ev) override {
        ++total_ticks_;
        return MockNode::Tick(bb, ev);
    }
    int total_ticks() const { return total_ticks_; }

private:
    int total_ticks_ = 0;
};

// --- Mock condition for testing logic composites + Pipeline scan without Lua ---

class MockCondition : public NodeCondition {
public:
    explicit MockCondition(NodeStatus status = NodeStatus::kSuccess)
        : NodeCondition("Mock"), status_(status) {}

    void set_status(NodeStatus s) { status_ = s; }
    int tick_count = 0;

    NodeStatus Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) override {
        ++tick_count;
        return status_;
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

TEST(InverterTest, FlipsSuccessToFailure) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    Inverter inv(1, "inv", std::unique_ptr<Node>(child));
    EXPECT_EQ(inv.Tick(bb, events), NodeStatus::kFailure);
}

TEST(InverterTest, FlipsFailureToSuccess) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    Inverter inv(1, "inv", std::unique_ptr<Node>(child));
    EXPECT_EQ(inv.Tick(bb, events), NodeStatus::kSuccess);
}

TEST(InverterTest, PassthroughRunning) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kRunning);
    Inverter inv(1, "inv", std::unique_ptr<Node>(child));
    EXPECT_EQ(inv.Tick(bb, events), NodeStatus::kRunning);
}

TEST(InverterTest, NoChildFails) {
    Blackboard bb;
    BtEventQueue events;
    Inverter inv(1, "inv", nullptr);
    EXPECT_EQ(inv.Tick(bb, events), NodeStatus::kFailure);
}

TEST(ForceSuccessTest, ForcesFailureToSuccess) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    ForceSuccess fs(1, "fs", std::unique_ptr<Node>(child));
    EXPECT_EQ(fs.Tick(bb, events), NodeStatus::kSuccess);
}

TEST(ForceSuccessTest, KeepsSuccess) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    ForceSuccess fs(1, "fs", std::unique_ptr<Node>(child));
    EXPECT_EQ(fs.Tick(bb, events), NodeStatus::kSuccess);
}

TEST(ForceSuccessTest, PassthroughRunning) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kRunning);
    ForceSuccess fs(1, "fs", std::unique_ptr<Node>(child));
    EXPECT_EQ(fs.Tick(bb, events), NodeStatus::kRunning);
}

TEST(ForceSuccessTest, NoChildFails) {
    Blackboard bb;
    BtEventQueue events;
    ForceSuccess fs(1, "fs", nullptr);
    EXPECT_EQ(fs.Tick(bb, events), NodeStatus::kFailure);
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
    engine->SetRoot(ParseJsonTree(
        R"({"type":"Selector","children":[{"type":"Script","source":"a.lua"},{"type":"Script","source":"b.lua"}]})"));
    EXPECT_TRUE(engine->IsLoaded());
}

TEST_F(BehaviorTreeEngineTest, StatusBeforeLoad) {
    EXPECT_EQ(engine->GetStatus(), "idle");
}

TEST_F(BehaviorTreeEngineTest, TickOnceOnLoadedTree) {
    engine->SetRoot(ParseJsonTree(
        R"({"type":"Selector","children":[{"type":"Script","source":"nonexistent.lua"}]})"));
    EXPECT_TRUE(engine->IsLoaded());

    auto status = engine->TickOnce();
    EXPECT_EQ(status, NodeStatus::kFailure);
}

TEST_F(BehaviorTreeEngineTest, StopResetsTree) {
    engine->SetRoot(ParseJsonTree(
        R"({"type":"Selector","children":[{"type":"Script","source":"nonexistent.lua"}]})"));
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

    engine->SetRoot(ParseJsonTree(
        R"({"type":"Selector","children":[{"type":"Script","source":"a.lua"}]})"));
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
        resource_provider = std::make_shared<MemoryResourceProvider>();
        rt = LuaRuntime::Builder()
            .WithResourceProvider(resource_provider)
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    // Register a root node JSON (referenced from Lua as root = "@root").
    void PutRoot(std::string json) { resource_provider->Put("root", std::move(json)); }
    // Register sensor definitions JSON (referenced as sensor_defs = "@sensors").
    void PutSensors(std::string json) { resource_provider->Put("sensors", std::move(json)); }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    std::shared_ptr<MemoryResourceProvider> resource_provider;
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
        return type(bt.init) == 'function'
            and type(bt.exec) == 'function'
            and type(bt.goto_path) == 'function'
            and type(bt.stop) == 'function'
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
    // New API has no JSON string; missing the required `root` table yields an error.
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({})
        return status, err or 'nil'
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("root"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, RunInvalidJsonReturnsSpecificError) {
    // A Script node with no source is rejected during parsing.
    PutRoot(R"({"type":"Script"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({root = "@root"})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("source"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, RunUnknownNodeType) {
    PutRoot(R"({"type":"UnknownType"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({root = "@root"})
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
    // x.lua does not exist → script init fails → bt.init returns nil + error.
    PutRoot(R"({"type":"Selector","children":[{"type":"Script","source":"x.lua"}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({root = "@root"})
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
    // No `root` field → error mentions "root".
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("root"), std::string::npos);
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
        resource_provider = std::make_shared<MemoryResourceProvider>();
        rt = LuaRuntime::Builder()
            .WithCodeProvider(std::make_shared<FileSystemCodeProvider>(
                std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts", tests_dir_ + "/sensors"}))
            .WithResourceProvider(resource_provider)
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    void PutRoot(std::string json) { resource_provider->Put("root", std::move(json)); }
    void PutSensors(std::string json) { resource_provider->Put("sensors", std::move(json)); }

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
    std::shared_ptr<MemoryResourceProvider> resource_provider;
    LuaRuntime::Ptr rt;
    std::string tests_dir_;
};

TEST_F(ScriptNodeIntegrationTest, SelfStateInEnterAndTick) {
    PutRoot(R"({"type":"Script","source":"scripts/bt_module.lua"})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ExitReasonAsParameter) {
    PutRoot(R"({"type":"Script","source":"scripts/check_reason.lua"})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ArgsPassedToEnter) {
    PutRoot(R"({"type":"Script","source":"scripts/with_args.lua","params":{"target":"enemy","damage":100}})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ArgsBoolType) {
    PutRoot(R"({"type":"Script","source":"scripts/bool_args.lua","params":{"enabled":true}})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, SubtreeParamsTemplatingPreservesTypes) {
    // A Subtree node forwards its `params` into the wrapped subtree JSON via
    // {{key}} placeholders. Whole-value placeholders preserve the param type
    // (string/number/bool flow through unchanged).
    PutRoot(R"({"type":"Subtree","source":"@sub","params":{"name":"lizhi","age":18,"active":true}})");
    resource_provider->Put("sub",
        R"({"type":"Script","source":"scripts/subtree_args.lua",)"
        R"("params":{"name":"{{name}}","age":"{{age}}","active":"{{active}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "@root"})
        bt.exec({interval = 10})
        return bb.get("sub_name"), bb.get("sub_age"), bb.get("sub_active")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 3u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "lizhi");
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 18);
    EXPECT_EQ(std::get<bool>(r.values[2]), true);
}

TEST_F(ScriptNodeIntegrationTest, SubtreeParamsPartialSubstitution) {
    // A placeholder embedded in a larger string is interpolated as text;
    // whole-value placeholders in the same params still preserve type.
    PutRoot(R"({"type":"Subtree","source":"@sub","params":{"name":"lizhi","age":18,"active":true}})");
    resource_provider->Put("sub",
        R"({"type":"Script","source":"scripts/subtree_args.lua",)"
        R"("params":{"name":"hello {{name}}","age":"{{age}}","active":"{{active}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "@root"})
        bt.exec({interval = 10})
        return bb.get("sub_name"), bb.get("sub_age"), bb.get("sub_active")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 3u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "hello lizhi");
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 18);
    EXPECT_EQ(std::get<bool>(r.values[2]), true);
}

TEST_F(ScriptNodeIntegrationTest, SubtreeParamsTemplatingCondition) {
    // Placeholders work in any string field, including a node's `condition`.
    // Here the Subtree's param selects which condition script the inner node
    // uses — "{{cond}}" resolves to the provided path.
    PutRoot(R"({"type":"Subtree","source":"@sub","params":{"cond":"scripts/cond_truthy.lua"}})");
    resource_provider->Put("sub",
        R"({"type":"Pipeline","children":[)"
        R"({"type":"Script","source":"scripts/no_args.lua","condition":{"type":"Script","source":"{{cond}}"}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    // Condition templated to cond_truthy -> met -> step runs -> success.
    EXPECT_EQ(*s, "success");
}

TEST_F(ScriptNodeIntegrationTest, SubtreeParamsNestedPassThrough) {
    // Outer params template the inner Subtree node's path AND params; those
    // params then template the inner subtree JSON. Values pass through every
    // level transparently.
    PutRoot(R"({"type":"Subtree","source":"@outer","params":{"role":"combat"}})");
    resource_provider->Put("outer",
        R"({"type":"Subtree","source":"@inner-{{role}}.json","params":{"who":"{{role}}"}})");
    resource_provider->Put("inner-combat.json",
        R"({"type":"Script","source":"scripts/subtree_args.lua","params":{"name":"{{who}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "@root"})
        bt.exec({interval = 10})
        return bb.get("sub_name")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "combat");
}

TEST_F(ScriptNodeIntegrationTest, TableParamPassedToEnter) {
    // An object-valued param arrives in Enter as a real Lua table: nested
    // field access works and scalar element types are preserved.
    PutRoot(R"({"type":"Script","source":"scripts/table_args.lua",)"
            R"("params":{"config":{"name":"lizhi","hp":100}}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "@root"})
        bt.exec({interval = 10})
        return bb.get("t_name"), bb.get("t_hp")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "lizhi");
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 100);
}

TEST_F(ScriptNodeIntegrationTest, SubtreeTableParamForwarded) {
    // A table param on the outer Subtree is forwarded (whole-value placeholder
    // preserves the object) and reaches the Script's Enter as a Lua table.
    PutRoot(R"({"type":"Subtree","source":"@sub",)"
            R"("params":{"profile":{"name":"lizhi","hp":100}}})");
    resource_provider->Put("sub",
        R"({"type":"Script","source":"scripts/table_args.lua","params":{"config":"{{profile}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "@root"})
        bt.exec({interval = 10})
        return bb.get("t_name"), bb.get("t_hp")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "lizhi");
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 100);
}

TEST_F(ScriptNodeIntegrationTest, SelfPersistsAcrossTicks) {
    PutRoot(R"({"type":"Script","source":"scripts/counter.lua"})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, NoArgsStillWorks) {
    PutRoot(R"({"type":"Script","source":"scripts/no_args.lua"})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ScriptNotFoundReturnsError) {
    PutRoot(R"({"type":"Script","source":"scripts/nonexistent.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({root = "@root"})
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
    PutRoot(R"({"type":"Script","source":"scripts/runtime_error.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
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
    PutRoot(R"({"type":"Script","source":"scripts/returns_failure.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(ScriptNodeIntegrationTest, InitErrorInSequenceStopsEarly) {
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","source":"scripts/nonexistent.lua"},)"
            R"({"type":"Script","source":"scripts/no_args.lua"}]})");
    auto r = AWAIT_BT(rt->RunScript(
        "local bt = require('bt')\n"
        "local status, err = bt.init({root = '@root'})\n"
        "return status, err\n"
    ));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("nonexistent.lua"), std::string::npos);
}

// --- Pipeline composite tests ---
//
// (Pipeline tests are rewritten for the scan-to-start + sequential model in
// the condition refactor; see the Pipeline + NodeCondition test sections.)

// --- bt lifecycle exec() max_step / timeout / interval Tests ---

class BtRunOptionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard = std::make_shared<Blackboard>();
        bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
        lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
        tests_dir_ = std::filesystem::absolute(
            std::filesystem::path(__FILE__).parent_path()).string();
        resource_provider = std::make_shared<MemoryResourceProvider>();
        rt = LuaRuntime::Builder()
            .WithCodeProvider(std::make_shared<FileSystemCodeProvider>(
                std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts", tests_dir_ + "/sensors"}))
            .WithResourceProvider(resource_provider)
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    void PutRoot(std::string json) { resource_provider->Put("root", std::move(json)); }
    void PutSensors(std::string json) { resource_provider->Put("sensors", std::move(json)); }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    std::shared_ptr<MemoryResourceProvider> resource_provider;
    LuaRuntime::Ptr rt;
    std::string tests_dir_;
};

TEST_F(BtRunOptionsTest, MaxStepStopsTree) {
    PutRoot(R"({"type":"Script","source":"scripts/run_forever.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({max_step = 2, interval = 10})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "timeout");
}

TEST_F(BtRunOptionsTest, MaxStepNotReachedTreeCompletes) {
    PutRoot(R"({"type":"Script","source":"scripts/run_3_ticks.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({max_step = 100, interval = 10})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(BtRunOptionsTest, TimeoutStopsTree) {
    PutRoot(R"({"type":"Script","source":"scripts/run_forever.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({timeout = 1, interval = 10})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "timeout");
}

TEST_F(BtRunOptionsTest, TimeoutNotReachedTreeCompletes) {
    PutRoot(R"({"type":"Script","source":"scripts/run_3_ticks.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({timeout = 60000, interval = 10})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(BtRunOptionsTest, IntervalAccepted) {
    PutRoot(R"({"type":"Script","source":"scripts/run_3_ticks.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(BtRunOptionsTest, CombinedMaxStepAndInterval) {
    PutRoot(R"({"type":"Script","source":"scripts/run_forever.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({max_step = 3, interval = 10})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "timeout");
}

// --- ForceFailure Tests ---

TEST(ForceFailureTest, ForcesSuccessToFailure) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    ForceFailure ff(1, "ff", std::unique_ptr<Node>(child));
    EXPECT_EQ(ff.Tick(bb, events), NodeStatus::kFailure);
}

TEST(ForceFailureTest, KeepsFailure) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    ForceFailure ff(1, "ff", std::unique_ptr<Node>(child));
    EXPECT_EQ(ff.Tick(bb, events), NodeStatus::kFailure);
}

TEST(ForceFailureTest, PassthroughRunning) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kRunning);
    ForceFailure ff(1, "ff", std::unique_ptr<Node>(child));
    EXPECT_EQ(ff.Tick(bb, events), NodeStatus::kRunning);
}

TEST(ForceFailureTest, NoChildFails) {
    Blackboard bb;
    BtEventQueue events;
    ForceFailure ff(1, "ff", nullptr);
    EXPECT_EQ(ff.Tick(bb, events), NodeStatus::kFailure);
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

TEST_F(BehaviorTreeLibraryTest, DumpPathsAfterRun) {
    PutRoot(R"({"type":"Wait","params":{"ms":999999}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        bt.exec({max_step=3, interval=1})
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
    PutRoot(R"({"type":"Wait","params":{"ms":999999}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", trace_paths=false})
        bt.exec({max_step=3, interval=1})
        local p = bt.dump_paths()
        return p.total_ticks, p.tracing
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 0);  // not collected
    EXPECT_EQ(std::get<bool>(r.values[1]), false);
}

TEST_F(BehaviorTreeLibraryTest, PathReportReturnsString) {
    PutRoot(R"({"type":"Wait","params":{"ms":999999}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        bt.exec({max_step=2, interval=1})
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
    PutRoot(R"({"type":"Sequence","name":"root","children":[)"
            R"({"type":"Wait","name":"w1","params":{"ms":99999}},)"
            R"({"type":"Wait","name":"w2","params":{"ms":99999}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local ok, err = bt.goto_path({'root', 'w2'})
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_GE(r.values.size(), 1u);
    EXPECT_EQ(std::get<bool>(r.values[0]), true);
}

TEST_F(BehaviorTreeLibraryTest, GotoPathNameNotFound) {
    PutRoot(R"({"type":"Sequence","name":"root","children":[)"
            R"({"type":"Wait","name":"w1","params":{"ms":99999}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local ok, err = bt.goto_path({'root', 'nope'})
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    EXPECT_NE(std::get<std::string>(r.values[1]).find("no child"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, GotoPathRejectsParallel) {
    PutRoot(R"({"type":"Parallel","name":"par","children":[)"
            R"({"type":"Wait","name":"w1","params":{"ms":99999}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local ok, err = bt.goto_path({'par', 'w1'})
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    EXPECT_NE(std::get<std::string>(r.values[1]).find("Parallel"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, RuntimePauseResumeFlag) {
    EXPECT_FALSE(rt->paused());
    rt->Pause();
    EXPECT_TRUE(rt->paused());
    rt->Resume();
    EXPECT_FALSE(rt->paused());
}

// --- Condition logic composite tests (MockCondition, no Lua) ---

TEST(ConditionLogicTest, AndAllSuccessIsSuccess) {
    AndCondition c;
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kSuccess));
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kSuccess));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(c.Tick(bb, ev), NodeStatus::kSuccess);
}

TEST(ConditionLogicTest, AndShortCircuitsOnFailure) {
    AndCondition c;
    auto* second = new MockCondition(NodeStatus::kSuccess);
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kFailure));
    c.AddChild(std::unique_ptr<MockCondition>(second));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(c.Tick(bb, ev), NodeStatus::kFailure);
    EXPECT_EQ(second->tick_count, 0);  // never evaluated
}

TEST(ConditionLogicTest, AndRunningPropagatesAndStops) {
    AndCondition c;
    auto* second = new MockCondition(NodeStatus::kSuccess);
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kRunning));
    c.AddChild(std::unique_ptr<MockCondition>(second));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(c.Tick(bb, ev), NodeStatus::kRunning);
    EXPECT_EQ(second->tick_count, 0);
}

TEST(ConditionLogicTest, OrAnySuccessIsSuccess) {
    OrCondition c;
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kFailure));
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kSuccess));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(c.Tick(bb, ev), NodeStatus::kSuccess);
}

TEST(ConditionLogicTest, OrShortCircuitsOnSuccess) {
    OrCondition c;
    auto* second = new MockCondition(NodeStatus::kFailure);
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kSuccess));
    c.AddChild(std::unique_ptr<MockCondition>(second));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(c.Tick(bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(second->tick_count, 0);
}

TEST(ConditionLogicTest, OrAllFailureIsFailure) {
    OrCondition c;
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kFailure));
    c.AddChild(std::make_unique<MockCondition>(NodeStatus::kFailure));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(c.Tick(bb, ev), NodeStatus::kFailure);
}

TEST(ConditionLogicTest, NotInvertsSuccessAndFailure) {
    Blackboard bb; BtEventQueue ev;
    NotCondition from_success(std::make_unique<MockCondition>(NodeStatus::kSuccess));
    EXPECT_EQ(from_success.Tick(bb, ev), NodeStatus::kFailure);
    NotCondition from_failure(std::make_unique<MockCondition>(NodeStatus::kFailure));
    EXPECT_EQ(from_failure.Tick(bb, ev), NodeStatus::kSuccess);
}

TEST(ConditionLogicTest, NotPassesRunningThrough) {
    NotCondition c(std::make_unique<MockCondition>(NodeStatus::kRunning));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(c.Tick(bb, ev), NodeStatus::kRunning);
}

// --- Pipeline scan + sequential unit tests (MockNode + MockCondition) ---

TEST(PipelineTest, NullConditionFirstChildSelectedThenSequential) {
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    auto* b = new MockNode(3, "b", NodeStatus::kSuccess);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    pipe.AddChild(std::unique_ptr<MockNode>(b));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(a->tick_count, 1);
    EXPECT_EQ(b->tick_count, 1);  // advanced same tick
}

TEST(PipelineTest, FirstMatchingConditionSelected) {
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    auto* b = new MockNode(3, "b", NodeStatus::kSuccess);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    pipe.AddChild(std::unique_ptr<MockNode>(b));
    pipe.children()[0]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    pipe.children()[1]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kSuccess));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(a->tick_count, 0);
    EXPECT_EQ(b->tick_count, 1);
}

TEST(PipelineTest, NoConditionMetTimesOutAtStep0) {
    // Scan finds no condition held → wait at step 0; $timeout (ticks) elapses
    // with condition still failing → fail (no previous step to back up to).
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a), /*timeout=*/2, /*retry=*/0);
    pipe.children()[0]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // scan no match → wait
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);  // 2 wait ticks → timeout at step 0
    EXPECT_EQ(a->tick_count, 0);
}

TEST(PipelineTest, WaitTimeoutWithoutRetryFails) {
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1), /*timeout=*/2, /*retry=*/0);
    pipe.children()[1]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // a0 runs; step1 waits
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);  // step1 timeout, no retry
    EXPECT_EQ(a1->tick_count, 0);
}

TEST(PipelineTest, RetryBacksUpAndSucceeds) {
    // Step1 condition appears only after action0 is re-run via a retry back-up.
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1), /*timeout=*/2, /*retry=*/1);
    auto* cond1 = new MockCondition(NodeStatus::kFailure);
    pipe.children()[1]->SetCondition(std::shared_ptr<MockCondition>(cond1));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // a0 runs; step1 waits
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // step1 timeout → back up, reset a0
    cond1->set_status(NodeStatus::kSuccess);             // now step1's condition holds
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kSuccess);  // re-run a0 → step1 → a1 → done
    EXPECT_EQ(a1->tick_count, 1);
}

TEST(PipelineTest, RetryExhaustedFails) {
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1), /*timeout=*/2, /*retry=*/1);
    pipe.children()[1]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // a0; step1 wait
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // timeout → back up, reset a0
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // re-run a0; step1 wait again
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);  // 2nd timeout, retry budget exhausted
    EXPECT_EQ(a1->tick_count, 0);
}

TEST(PipelineTest, RetryFailsIfPrevConditionLost) {
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    auto* cond0 = new MockCondition(NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1), /*timeout=*/2, /*retry=*/1);
    pipe.children()[0]->SetCondition(std::shared_ptr<MockCondition>(cond0));
    pipe.children()[1]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // scan step0; a0 runs; step1 waits
    cond0->set_status(NodeStatus::kFailure);             // previous condition now lost
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);  // back-up gate fails
    EXPECT_EQ(a1->tick_count, 0);
}

TEST(PipelineTest, RunningConditionKeepsScanPhase) {
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    auto cond = std::make_shared<MockCondition>(NodeStatus::kRunning);
    pipe.children()[0]->SetCondition(cond);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // still scanning
    EXPECT_EQ(a->tick_count, 0);
    cond->set_status(NodeStatus::kSuccess);
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kSuccess);  // now selected + runs
    EXPECT_EQ(a->tick_count, 1);
}

TEST(PipelineTest, ChildFailureFailsPipeline) {
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kFailure);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);
}

TEST(PipelineTest, ResetReScans) {
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    Blackboard bb; BtEventQueue ev;
    ASSERT_EQ(pipe.Tick(bb, ev), NodeStatus::kSuccess);
    pipe.Reset();  // clears started_ + child state (tick_count -> 0)
    ASSERT_EQ(pipe.Tick(bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(a->tick_count, 1);  // re-ran after reset (scan phase re-entered)
}

// --- RollIntInRange + Pipeline $timeout/$retry range-randomization tests ---
//
// $timeout/$retry may be a fixed int (lo==hi) or a [lo,hi] range; the Pipeline
// rolls one value per step per run. The fixed cases must match the legacy
// single-value behaviour; the range cases must keep the resolved value inside
// the bounds across many runs.

TEST(RollIntInRangeTest, DegenerateRangeReturnsLo) {
    std::mt19937 g(123);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(RollIntInRange(5, 5, g), 5);   // lo==hi
        EXPECT_EQ(RollIntInRange(7, 3, g), 7);   // hi<lo → lo
        EXPECT_EQ(RollIntInRange(0, 0, g), 0);
    }
}

TEST(RollIntInRangeTest, StaysWithinRangeAndVisitsEndpoints) {
    std::mt19937 g(42);
    const int lo = 3, hi = 9;
    int seen_min = hi, seen_max = lo;
    for (int i = 0; i < 3000; ++i) {
        int v = RollIntInRange(lo, hi, g);
        ASSERT_GE(v, lo);
        ASSERT_LE(v, hi);
        seen_min = std::min(seen_min, v);
        seen_max = std::max(seen_max, v);
    }
    EXPECT_EQ(seen_min, lo);  // uniform draw hits both endpoints over 3000 tries
    EXPECT_EQ(seen_max, hi);
}

TEST(PipelineTest, RangeTimeoutCollapsesToFixedValue) {
    // [2,2] must behave exactly like the legacy $timeout=2 (timeout, no retry).
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a), /*lo=*/2, /*hi=*/2, /*retry=*/0, /*retry=*/0);
    pipe.children()[0]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // scan no match → wait, tick 1
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);  // tick 2 → timeout at step 0
    EXPECT_EQ(a->tick_count, 0);
}

TEST(PipelineTest, RangeTimeoutResolvesWithinBounds) {
    // $timeout=[1,5]: across many runs the actual wait (ticks to timeout) must
    // stay within [1,5] and reach both endpoints (proves a real roll, not a clamp).
    const int lo = 1, hi = 5;
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a), lo, hi, 0, 0);
    pipe.children()[0]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));

    int seen_min = hi, seen_max = lo;
    for (int run = 0; run < 80; ++run) {
        pipe.Reset();
        Blackboard bb; BtEventQueue ev;
        int ticks = 0;
        NodeStatus s = NodeStatus::kRunning;
        while (s == NodeStatus::kRunning) {
            s = pipe.Tick(bb, ev);  // each tick spends one wait unit; failure on the T-th
            ++ticks;
        }
        ASSERT_EQ(s, NodeStatus::kFailure);  // step 0, no retry → timeout fails
        ASSERT_GE(ticks, lo);
        ASSERT_LE(ticks, hi);
        seen_min = std::min(seen_min, ticks);
        seen_max = std::max(seen_max, ticks);
    }
    EXPECT_EQ(seen_min, lo);
    EXPECT_EQ(seen_max, hi);
    EXPECT_EQ(a->tick_count, 0);
}

TEST(PipelineTest, RangeRetryCollapsesToFixedValue) {
    // retry=[1,1] must behave like the legacy $retry=1 (one back-up, then fail).
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1), /*timeout=*/2, /*timeout=*/2,
                 /*retry_lo=*/1, /*retry_hi=*/1);
    pipe.children()[1]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // a0 runs; step1 waits
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // step1 timeout → back up, reset a0
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // re-run a0; step1 wait again
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);  // 2nd timeout, retry budget exhausted
    EXPECT_EQ(a1->tick_count, 0);
}

TEST(PipelineTest, RangeRetryResolvesWithinBounds) {
    // retry=[1,4]: the number of back-up re-runs of a0 must land in [1,4]. a0 is
    // ticked once initially + once per back-up, so (total_ticks delta) - 1 gives
    // the back-up count for each run.
    const int lo = 1, hi = 4;
    Pipeline pipe(1, "pipe");
    auto* a0 = new CountingMockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(static_cast<MockNode*>(a0)), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1), /*timeout_lo=*/1, /*timeout_hi=*/1, lo, hi);
    pipe.children()[1]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));

    int seen_min = hi, seen_max = lo;
    for (int run = 0; run < 80; ++run) {
        pipe.Reset();
        Blackboard bb; BtEventQueue ev;
        int before = a0->total_ticks();
        NodeStatus s = NodeStatus::kRunning;
        while (s == NodeStatus::kRunning) {
            s = pipe.Tick(bb, ev);
        }
        ASSERT_EQ(s, NodeStatus::kFailure);
        int back_ups = (a0->total_ticks() - before) - 1;  // initial run + one per back-up
        ASSERT_GE(back_ups, lo);
        ASSERT_LE(back_ups, hi);
        seen_min = std::min(seen_min, back_ups);
        seen_max = std::max(seen_max, back_ups);
    }
    EXPECT_EQ(seen_min, lo);
    EXPECT_EQ(seen_max, hi);
}

// --- Node guard (condition) + interruption tests ---

TEST(NodeGuardTest, ConditionGatesEntryAndInterruptsOnFailure) {
    // With abort=Self, a node's condition is a continuous guard: while it holds
    // the node runs; the moment it goes Failure the running work is interrupted.
    auto* mock = new MockNode(1, "n", NodeStatus::kRunning);
    std::unique_ptr<Node> node(mock);
    auto cond = std::make_shared<MockCondition>(NodeStatus::kSuccess);
    cond->set_abort(AbortMode::kSelf);
    node->SetCondition(cond);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(node->TickAndRecord(bb, ev), NodeStatus::kRunning);  // guard met -> run
    EXPECT_FALSE(mock->aborted);
    cond->set_status(NodeStatus::kFailure);                        // guard lost
    EXPECT_EQ(node->TickAndRecord(bb, ev), NodeStatus::kFailure);  // interrupted
    EXPECT_TRUE(mock->aborted);
}

TEST(NodeGuardTest, NoneAbortDoesNotInterrupt) {
    // Default abort=None: the condition is NOT monitored — a running node keeps
    // running even after its condition goes Failure.
    auto* mock = new MockNode(1, "n", NodeStatus::kRunning);
    std::unique_ptr<Node> node(mock);
    auto cond = std::make_shared<MockCondition>(NodeStatus::kSuccess);  // abort=None
    node->SetCondition(cond);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(node->TickAndRecord(bb, ev), NodeStatus::kRunning);
    cond->set_status(NodeStatus::kFailure);
    EXPECT_EQ(node->TickAndRecord(bb, ev), NodeStatus::kRunning);  // not interrupted
    EXPECT_FALSE(mock->aborted);
}

TEST(NodeGuardTest, AsyncRunningUsesLastResult) {
    // An async guard returning Running is treated as its last terminal result,
    // so a previously-met guard doesn't spuriously interrupt the subordinate.
    auto* mock = new MockNode(1, "n", NodeStatus::kRunning);
    std::unique_ptr<Node> node(mock);
    auto cond = std::make_shared<MockCondition>(NodeStatus::kSuccess);
    cond->set_abort(AbortMode::kSelf);
    node->SetCondition(cond);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(node->TickAndRecord(bb, ev), NodeStatus::kRunning);  // guard Success -> run
    cond->set_status(NodeStatus::kRunning);                        // async mid-re-eval
    EXPECT_EQ(node->TickAndRecord(bb, ev), NodeStatus::kRunning);  // stale Success -> continue
    EXPECT_FALSE(mock->aborted);
    cond->set_status(NodeStatus::kFailure);                        // guard actually failed
    EXPECT_EQ(node->TickAndRecord(bb, ev), NodeStatus::kFailure);  // now interrupted
    EXPECT_TRUE(mock->aborted);
}

TEST(PipelineTest, GuardInterruptsActionWhenConditionLost) {
    // With abort=Self on the step's condition, while the action runs the
    // condition is monitored; if the page is lost (Failure) the action is
    // aborted and the step fails.
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kRunning);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), 0, 0);
    auto* cond0 = new MockCondition(NodeStatus::kSuccess);
    cond0->set_abort(AbortMode::kSelf);
    pipe.children()[0]->SetCondition(std::shared_ptr<MockCondition>(cond0));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kRunning);  // a0 running under its guard
    EXPECT_FALSE(a0->aborted);
    cond0->set_status(NodeStatus::kFailure);             // page lost
    EXPECT_EQ(pipe.Tick(bb, ev), NodeStatus::kFailure);  // guard interrupts a0 -> step fails
    EXPECT_TRUE(a0->aborted);
}

TEST(AbortTest, LowerPriorityPreemptsRunningSibling) {
    // Selector: high-priority child gated out (cond false) -> low runs. When the
    // high cond flips true, abort=LowerPriority preempts the low branch.
    BehaviorTreeEngine engine;
    auto* a_hi = new MockNode(2, "high", NodeStatus::kRunning);
    auto* a_lo = new MockNode(3, "low", NodeStatus::kRunning);
    auto sel = std::make_unique<Selector>(1, "sel");
    auto cond_hi = std::make_shared<MockCondition>(NodeStatus::kFailure);
    cond_hi->set_abort(AbortMode::kLowerPriority);
    sel->AddChild(std::unique_ptr<MockNode>(a_hi));
    sel->children()[0]->SetCondition(cond_hi);
    sel->AddChild(std::unique_ptr<MockNode>(a_lo));
    engine.SetRoot(std::move(sel));

    EXPECT_EQ(engine.TickOnce(), NodeStatus::kRunning);  // high gated out -> low runs
    EXPECT_EQ(a_hi->tick_count, 0);
    EXPECT_EQ(a_lo->tick_count, 1);

    cond_hi->set_status(NodeStatus::kSuccess);           // high now wants priority
    EXPECT_EQ(engine.TickOnce(), NodeStatus::kRunning);  // preempts low -> high runs
    EXPECT_TRUE(a_lo->aborted);
    EXPECT_EQ(a_hi->tick_count, 1);
}

TEST(AbortTest, ConditionGatesSelectorEntry) {
    // abort=None still gates entry: a Selector skips a child whose condition
    // is Failure, even though None does no reactive monitoring. (Running action
    // so the tree stays active — TickOnce resets state on terminal.)
    BehaviorTreeEngine engine;
    auto* a_skip = new MockNode(2, "skip", NodeStatus::kRunning);
    auto* a_run = new MockNode(3, "run", NodeStatus::kRunning);
    auto sel = std::make_unique<Selector>(1, "sel");
    sel->AddChild(std::unique_ptr<MockNode>(a_skip));
    sel->children()[0]->SetCondition(std::make_shared<MockCondition>(NodeStatus::kFailure));
    sel->AddChild(std::unique_ptr<MockNode>(a_run));
    engine.SetRoot(std::move(sel));

    EXPECT_EQ(engine.TickOnce(), NodeStatus::kRunning);  // gated child skipped, second runs
    EXPECT_EQ(a_skip->tick_count, 0);
    EXPECT_EQ(a_run->tick_count, 1);
}

// --- ScriptCondition + Pipeline integration (Lua, via JSON parser) ---

TEST_F(ScriptNodeIntegrationTest, ScriptConditionTruthySelectsAndRuns) {
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua","condition":{"type":"Script","source":"scripts/cond_truthy.lua"}}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10, max_step = 20})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, PipelineScansPastFailingCondition) {
    // Step 1's condition is never met; step 2's is. Pipeline starts at step 2.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua","condition":{"type":"Script","source":"scripts/cond_falsy.lua"}},)"
            R"({"type":"Script","source":"scripts/no_args.lua","condition":{"type":"Script","source":"scripts/cond_truthy.lua"}}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10, max_step = 20})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, PipelineNoConditionMetFails) {
    // cond_falsy never holds → scan finds no match → wait at step 0; $timeout
    // (ticks) elapses → fail (no previous step to back up to).
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("condition":{"type":"Script","source":"scripts/cond_falsy.lua"},"$timeout":3}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status = bt.exec({interval = 10, max_step = 20})
        return status
    )");
    EXPECT_EQ(status, "failure");
}

TEST_F(ScriptNodeIntegrationTest, PipelineArrayTimeoutEquivalentToScalar) {
    // $timeout as a [lo,hi] array must parse and behave like the scalar 3 above.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("condition":{"type":"Script","source":"scripts/cond_falsy.lua"},"$timeout":[3,3]}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status = bt.exec({interval = 10, max_step = 20})
        return status
    )");
    EXPECT_EQ(status, "failure");
}

TEST_F(ScriptNodeIntegrationTest, ConditionAndOrNotComposition) {
    // (cond_falsy OR cond_truthy) AND NOT cond_falsy -> met -> step runs.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua","condition":)"
            R"({"type":"And","children":[)"
            R"({"type":"Or","children":[)"
            R"({"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"({"type":"Script","source":"scripts/cond_truthy.lua"}]},)"
            R"({"type":"Not","child":{"type":"Script","source":"scripts/cond_falsy.lua"}}]}}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        local status, err = bt.exec({interval = 10, max_step = 20})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

// --- E2E simulated automation scenarios (Lua + bt.exec, blackboard = page state) ---
//
// Reusable fixtures (tests/scripts/e2e_*.lua): e2e_when (cond: bb[key]==value),
// e2e_goto (action: navigate, flaky), e2e_set (action: bb[key]=value),
// e2e_run (action: long-running, optional mid-run bb write + Exit tracking),
// e2e_decay (cond: holds N ticks then fails).

// Scenario 1 — checkout flow: Pipeline (scan + wait + retry back-up) with a
// parameterized Subtree for one step. A flaky navigation forces a retry back-up.
TEST_F(ScriptNodeIntegrationTest, E2E_CheckoutPipelineWithSubtreeAndRetry) {
    // Subtree wraps the navigate action; {{to}}/{{flaky}} templated by the step.
    resource_provider->Put("nav",
        R"({"type":"Script","source":"scripts/e2e_goto.lua",)"
        R"("params":{"to":"{{to}}","flaky":"{{flaky}}"}})");
    PutRoot(R"({"type":"Pipeline","children":[)"
            // step 1: on home -> open cart (flaky 1st attempt) via Subtree
            R"({"type":"Subtree","source":"@nav","params":{"to":"cart","flaky":1},)"
            R"("condition":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"home"}}},)"
            // step 2: on cart -> go to checkout; wait for cart with retry back-up
            R"({"type":"Script","source":"scripts/e2e_goto.lua","params":{"to":"checkout"},)"
            R"("condition":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"cart"}},)"
            R"("$timeout":3,"$retry":1},)"
            // step 3: on checkout -> mark done
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"done","value":true},)"
            R"("condition":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"checkout"}}})"
            R"(]})");
    blackboard->Set("page", LuaValue(std::string("home")));  // start on home

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "success");
    EXPECT_EQ(*blackboard->Get("done"), LuaValue(true));                       // flow completed
    EXPECT_EQ(*blackboard->Get("goto_cart"), LuaValue(static_cast<int64_t>(2)));  // retried once
}

// Scenario 2 — condition interrupt (Self): a long form-filling action runs under
// a "still on form page" guard that decays. When the page disappears mid-fill,
// the guard aborts the action and the Pipeline fails.
TEST_F(ScriptNodeIntegrationTest, E2E_SelfAbortInterruptsLongAction) {
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/e2e_run.lua","params":{"ticks":10,"exit_key":"form_exit"},)"
            R"("condition":{"type":"Script","source":"scripts/e2e_decay.lua","params":{"hold":3},"abort":"Self"}}]})");

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "failure");                                  // guard aborted the action
    ASSERT_TRUE(blackboard->Has("form_exit"));
    EXPECT_EQ(*blackboard->Get("form_exit"), LuaValue(std::string("aborted")));
}

// Scenario 3 — LowerPriority preemption: a worker runs until it triggers an
// alert popup; the high-priority alert handler (LowerPriority) preempts the
// worker, dismisses the alert, and the Selector succeeds.
TEST_F(ScriptNodeIntegrationTest, E2E_LowerPriorityPreemptsWorker) {
    PutRoot(R"({"type":"Selector","children":[)"
            // high priority: dismiss alert when one is visible
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"alert_visible","value":false},)"
            R"("condition":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"alert_visible","value":true},"abort":"LowerPriority"}},)"
            // low priority: worker that triggers an alert on tick 2
            R"({"type":"Script","source":"scripts/e2e_run.lua",)"
            R"("params":{"ticks":0,"set_at":2,"set_key":"alert_visible","set_val":true,"exit_key":"worker_exit"}}]})");

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "@root"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "success");                                  // alert dismissed
    ASSERT_TRUE(blackboard->Has("worker_exit"));
    EXPECT_EQ(*blackboard->Get("worker_exit"), LuaValue(std::string("aborted")));  // preempted
    EXPECT_EQ(*blackboard->Get("alert_visible"), LuaValue(false));               // cleared
}
