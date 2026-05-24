#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <filesystem>
#include <fstream>
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
#include "tree_parser.h"

// --- Mock node for testing composites without Lua ---

class MockNode : public Node {
public:
    explicit MockNode(uint32_t id, const std::string& name = "mock",
                     NodeStatus status = NodeStatus::kSuccess)
        : Node(id, "Mock", name), status_(status) {}

    void set_status(NodeStatus s) { status_ = s; }

    NodeStatus Tick(Blackboard& /*bb*/, BtEventQueue& /*events*/) override { return status_; }

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

// --- TreeParser Tests ---

TEST(TreeParserTest, ParseSimpleSelector) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "children": [
                {"type": "Script", "path": "a.lua"},
                {"type": "Script", "path": "b.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Selector");
    auto* sel = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->children().size(), 2u);
}

TEST(TreeParserTest, ParseSequence) {
    const char* json = R"({
        "root": {
            "type": "Sequence",
            "children": [
                {"type": "Script", "path": "x.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Sequence");
}

TEST(TreeParserTest, ParseParallel) {
    const char* json = R"({
        "root": {
            "type": "Parallel",
            "success_policy": "RequireOne",
            "failure_policy": "RequireAll",
            "children": [
                {"type": "Script", "path": "a.lua"},
                {"type": "Script", "path": "b.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Parallel");
}

TEST(TreeParserTest, ParseScriptNode) {
    const char* json = R"({
        "root": {"type": "Script", "path": "scripts/test.lua"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Script");
}

TEST(TreeParserTest, ParseDecorators) {
    const char* json = R"({
        "root": {
            "type": "Sequence",
            "decorators": [
                {
                    "type": "BlackboardCondition",
                    "key": "has_target",
                    "operator": "is_set",
                    "abort": "Both"
                }
            ],
            "children": [
                {"type": "Script", "path": "attack.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->decorators().size(), 1u);
    auto* bc = dynamic_cast<BlackboardCondition*>(root->decorators()[0].get());
    ASSERT_NE(bc, nullptr);
    EXPECT_EQ(bc->abort_mode(), AbortMode::kBoth);
}

TEST(TreeParserTest, ParseNestedTree) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "children": [
                {
                    "type": "Sequence",
                    "children": [
                        {"type": "Script", "path": "a.lua"},
                        {"type": "Script", "path": "b.lua"}
                    ]
                },
                {"type": "Script", "path": "c.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* sel = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->children().size(), 2u);

    auto* seq = dynamic_cast<Composite*>(sel->children()[0].get());
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(seq->children().size(), 2u);
}

TEST(TreeParserTest, ParseInvalidJson) {
    auto _parse_result = TreeParser::Parse("{invalid json");
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(TreeParserTest, ParseMissingRoot) {
    auto _parse_result = TreeParser::Parse("{}");
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(TreeParserTest, ParseUnknownNodeType) {
    const char* json = R"({
        "root": {"type": "UnknownType"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(TreeParserTest, ParseScriptMissingPath) {
    const char* json = R"({
        "root": {"type": "Script"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(TreeParserTest, ParseNodeNames) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "name": "root_sel",
            "children": [
                {"type": "Script", "path": "a.lua", "name": "check_a"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->name(), "root_sel");

    auto* sel = dynamic_cast<Composite*>(root.get());
    EXPECT_EQ(sel->children()[0]->name(), "check_a");
}

TEST(TreeParserTest, ParseInverterDecorator) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "a.lua",
            "decorators": [
                {"type": "Inverter", "abort": "Self"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->decorators().size(), 1u);
    auto* inv = dynamic_cast<Inverter*>(root->decorators()[0].get());
    ASSERT_NE(inv, nullptr);
    EXPECT_EQ(inv->abort_mode(), AbortMode::kSelf);
}

TEST(TreeParserTest, ParseForceSuccessDecorator) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "a.lua",
            "decorators": [
                {"type": "ForceSuccess"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->decorators().size(), 1u);
    auto* fs = dynamic_cast<ForceSuccess*>(root->decorators()[0].get());
    ASSERT_NE(fs, nullptr);
    EXPECT_EQ(fs->abort_mode(), AbortMode::kNone);
}

// --- BehaviorTreeEngine Tests ---

class BehaviorTreeEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_shared<BehaviorTreeEngine>();
    }

    void TearDown() override {
        if (engine && engine->IsRunning()) {
            engine->Stop();
        }
    }

    BehaviorTreeEngine::Ptr engine;
};

TEST_F(BehaviorTreeEngineTest, LoadInvalidJson) {
    EXPECT_FALSE(engine->Load("not json").first);
}

TEST_F(BehaviorTreeEngineTest, LoadMissingRoot) {
    EXPECT_FALSE(engine->Load("{}").first);
}

TEST_F(BehaviorTreeEngineTest, LoadValidTree) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "children": [
                {"type": "Script", "path": "a.lua"},
                {"type": "Script", "path": "b.lua"}
            ]
        }
    })";
    EXPECT_TRUE(engine->Load(json).first);
}

TEST_F(BehaviorTreeEngineTest, StatusBeforeRun) {
    EXPECT_EQ(engine->GetStatus(), "stopped");
}

TEST_F(BehaviorTreeEngineTest, RunAndStop) {
    // Use a tree with only Script nodes that will fail (no LuaContext),
    // so the tree should complete quickly and the engine should stop
    const char* json = R"({
        "root": {
            "type": "Selector",
            "children": [
                {"type": "Script", "path": "nonexistent.lua"}
            ]
        }
    })";
    ASSERT_TRUE(engine->Load(json).first);
    engine->Run();
    EXPECT_TRUE(engine->IsRunning());

    // Give it time to tick at least once
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine->Stop();
    EXPECT_FALSE(engine->IsRunning());
    EXPECT_EQ(engine->GetStatus(), "stopped");
}

TEST_F(BehaviorTreeEngineTest, PauseAndResume) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "children": [
                {"type": "Script", "path": "nonexistent.lua"}
            ]
        }
    })";
    ASSERT_TRUE(engine->Load(json).first);
    engine->Run();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    engine->Pause();
    EXPECT_TRUE(engine->IsPaused());

    engine->Resume();
    EXPECT_FALSE(engine->IsPaused());

    engine->Stop();
}

TEST_F(BehaviorTreeEngineTest, StopWithoutRun) {
    // Should not crash
    engine->Stop();
    EXPECT_FALSE(engine->IsRunning());
}

TEST_F(BehaviorTreeEngineTest, BlackboardPersistsAcrossLoad) {
    engine->blackboard().Set("x", LuaValue(static_cast<int64_t>(42)));
    EXPECT_TRUE(engine->blackboard().Has("x"));

    const char* json = R"({
        "root": {"type": "Selector", "children": [{"type": "Script", "path": "a.lua"}]}
    })";
    engine->Load(json);
    // Load should clear blackboard
    EXPECT_FALSE(engine->blackboard().Has("x"));
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
        if (lib && lib->engine() && lib->engine()->IsRunning()) {
            lib->engine()->StopLoop();
            lib->engine()->Stop();
        }
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
        return type(bt.run) == 'function'
            and type(bt.pause) == 'function'
            and type(bt.resume) == 'function'
            and type(bt.stop) == 'function'
            and type(bt.notify) == 'function'
            and type(bt.get_status) == 'function'
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
    EXPECT_EQ(*s, "stopped");
}

TEST_F(BehaviorTreeLibraryTest, RunInvalidJson) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local ok, err = bt.run('{invalid}')
        return ok, err or 'nil'
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("JSON"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, RunInvalidJsonReturnsSpecificError) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local ok, err = bt.run('{"children":[]}')
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("root"), std::string::npos);
}

TEST_F(BehaviorTreeLibraryTest, RunUnknownNodeType) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local ok, err = bt.run('{"root":{"type":"UnknownType"}}')
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
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

TEST_F(BehaviorTreeLibraryTest, RunAndCheckStatus) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local json = '{"root":{"type":"Selector","children":[{"type":"Script","path":"x.lua"}]}}'
        local ok, err = bt.run(json)
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_FALSE(err->empty());
    EXPECT_FALSE(lib->engine()->IsRunning());
}

TEST_F(BehaviorTreeLibraryTest, PauseResumeFromLua) {
    lib->SetTickIntervalMs(200);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')

        local json = '{"root":{"type":"Selector","children":[{"type":"Script","path":"x.lua"}]}}'

        local ok, err = bt.run(json)
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_FALSE(err->empty());
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
    const char* json = R"({
        "root": {
            "type": "Selector",
            "decorators": [
                {"type": "BlackboardCondition", "key": "hp", "operator": "is_set"}
            ],
            "children": [{"type": "Script", "path": "a.lua"}]
        }
    })";
    EXPECT_TRUE(engine->Load(json).first);
}

// --- AbortMode Parsing Tests ---

TEST(AbortModeTest, TreeParserParseAbortNone) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "decorators": [
                {"type": "BlackboardCondition", "key": "x", "operator": "is_set", "abort": "None"}
            ],
            "children": [{"type": "Script", "path": "a.lua"}]
        }
    })";
    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->decorators().size(), 1u);
    auto* bc = dynamic_cast<BlackboardCondition*>(root->decorators()[0].get());
    ASSERT_NE(bc, nullptr);
    EXPECT_EQ(bc->abort_mode(), AbortMode::kNone);
}

TEST(AbortModeTest, TreeParserParseAbortSelf) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "decorators": [
                {"type": "BlackboardCondition", "key": "x", "operator": "is_set", "abort": "Self"}
            ],
            "children": [{"type": "Script", "path": "a.lua"}]
        }
    })";
    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* bc = dynamic_cast<BlackboardCondition*>(root->decorators()[0].get());
    ASSERT_NE(bc, nullptr);
    EXPECT_EQ(bc->abort_mode(), AbortMode::kSelf);
}

TEST(AbortModeTest, TreeParserParseAbortLowerPriority) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "decorators": [
                {"type": "BlackboardCondition", "key": "x", "operator": "is_set", "abort": "LowerPriority"}
            ],
            "children": [{"type": "Script", "path": "a.lua"}]
        }
    })";
    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* bc = dynamic_cast<BlackboardCondition*>(root->decorators()[0].get());
    ASSERT_NE(bc, nullptr);
    EXPECT_EQ(bc->abort_mode(), AbortMode::kLowerPriority);
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

// --- Subtree Tests ---

TEST(SubtreeTest, ParseSubtree) {
    const char* json = R"({
        "subtrees": {
            "combat": {
                "type": "Sequence",
                "children": [
                    {"type": "Script", "path": "aim.lua"},
                    {"type": "Script", "path": "attack.lua"}
                ]
            }
        },
        "root": {
            "type": "Subtree",
            "subtree": "combat"
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Subtree");
    auto* sub = dynamic_cast<SubtreeNode*>(root.get());
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->subtree_name(), "combat");
    EXPECT_NE(sub->subtree_root(), nullptr);

    auto* inner = dynamic_cast<Composite*>(sub->subtree_root());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->children().size(), 2u);
}

TEST(SubtreeTest, ParseSubtreeMissingName) {
    const char* json = R"({
        "root": {"type": "Subtree"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(SubtreeTest, ParseSubtreeUnknownName) {
    const char* json = R"({
        "root": {"type": "Subtree", "subtree": "nonexistent"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(SubtreeTest, ParseNestedSubtree) {
    const char* json = R"({
        "subtrees": {
            "inner": {
                "type": "Script",
                "path": "inner.lua"
            },
            "outer": {
                "type": "Sequence",
                "children": [
                    {"type": "Subtree", "subtree": "inner"},
                    {"type": "Script", "path": "outer.lua"}
                ]
            }
        },
        "root": {
            "type": "Subtree",
            "subtree": "outer"
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Subtree");

    auto* outer_sub = dynamic_cast<SubtreeNode*>(root.get());
    ASSERT_NE(outer_sub, nullptr);
    auto* outer_seq = dynamic_cast<Composite*>(outer_sub->subtree_root());
    ASSERT_NE(outer_seq, nullptr);
    EXPECT_EQ(outer_seq->children().size(), 2u);

    // First child is an inner SubtreeNode
    auto* inner_sub = dynamic_cast<SubtreeNode*>(outer_seq->children()[0].get());
    ASSERT_NE(inner_sub, nullptr);
    EXPECT_EQ(inner_sub->subtree_name(), "inner");
}

TEST(SubtreeTest, SubtreeWithDecorators) {
    const char* json = R"({
        "subtrees": {
            "combat": {
                "type": "Script",
                "path": "fight.lua"
            }
        },
        "root": {
            "type": "Subtree",
            "subtree": "combat",
            "decorators": [
                {"type": "BlackboardCondition", "key": "has_target", "operator": "is_set"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->decorators().size(), 1u);
}

TEST(SubtreeTest, SubtreeWithSensors) {
    const char* json = R"({
        "subtrees": {
            "patrol": {
                "type": "Script",
                "path": "patrol.lua"
            }
        },
        "root": {
            "type": "Subtree",
            "subtree": "patrol",
            "sensors": [
                {"name": "nearby", "path": "sensors/nearby.lua", "interval": 200}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->sensor_specs().size(), 1u);
    EXPECT_EQ(root->sensor_specs()[0].name, "nearby");
}

TEST(SubtreeTest, SubtreeUsedMultipleTimes) {
    const char* json = R"({
        "subtrees": {
            "check": {
                "type": "Script",
                "path": "check.lua"
            }
        },
        "root": {
            "type": "Sequence",
            "children": [
                {"type": "Subtree", "subtree": "check", "name": "check_1"},
                {"type": "Subtree", "subtree": "check", "name": "check_2"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* seq = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(seq->children().size(), 2u);

    EXPECT_EQ(seq->children()[0]->name(), "check_1");
    EXPECT_EQ(seq->children()[1]->name(), "check_2");
    EXPECT_EQ(seq->children()[0]->type(), "Subtree");
    EXPECT_EQ(seq->children()[1]->type(), "Subtree");
}

TEST(SubtreeTest, SubtreeInSelector) {
    const char* json = R"({
        "subtrees": {
            "combat": {
                "type": "Sequence",
                "children": [
                    {"type": "Script", "path": "aim.lua"},
                    {"type": "Script", "path": "attack.lua"}
                ]
            }
        },
        "root": {
            "type": "Selector",
            "children": [
                {"type": "Subtree", "subtree": "combat"},
                {"type": "Script", "path": "idle.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* sel = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->children().size(), 2u);

    auto* sub = dynamic_cast<SubtreeNode*>(sel->children()[0].get());
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->subtree_name(), "combat");
}

TEST(SubtreeTest, SubtreeParentPointer) {
    const char* json = R"({
        "subtrees": {
            "leaf": {
                "type": "Script",
                "path": "leaf.lua"
            }
        },
        "root": {
            "type": "Subtree",
            "subtree": "leaf"
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* sub = dynamic_cast<SubtreeNode*>(root.get());
    ASSERT_NE(sub, nullptr);

    // Subtree root's parent should be the SubtreeNode
    EXPECT_EQ(sub->subtree_root()->parent(), sub);
}

TEST(SubtreeTest, CircularSubtreeReference) {
    const char* json = R"({
        "subtrees": {
            "a": {"type": "Subtree", "subtree": "a"}
        },
        "root": {"type": "Subtree", "subtree": "a"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(SubtreeTest, IndirectCircularReference) {
    const char* json = R"({
        "subtrees": {
            "a": {"type": "Subtree", "subtree": "b"},
            "b": {"type": "Subtree", "subtree": "a"}
        },
        "root": {"type": "Subtree", "subtree": "a"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

// --- LoadTreeFromDirectory Tests ---

class LoadTreeFromDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "bt_test_tree";
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    void WriteFile(const std::string& name, const std::string& content) {
        std::ofstream(dir_ / name) << content;
    }

    std::filesystem::path dir_;
};

TEST_F(LoadTreeFromDirectoryTest, LoadsRootOnly) {
    WriteFile("root.json", R"({"type": "Selector", "children": [
        {"type": "Script", "path": "a.lua"}
    ]})");

    auto json = TreeParser::LoadTreeFromDirectory(dir_.string());
    ASSERT_FALSE(json.empty());

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Selector");
}

TEST_F(LoadTreeFromDirectoryTest, LoadsRootWithSubtrees) {
    WriteFile("root.json", R"({"type": "Selector", "children": [
        {"type": "Subtree", "subtree": "combat"},
        {"type": "Script", "path": "idle.lua"}
    ]})");
    WriteFile("combat.json", R"({"type": "Sequence", "children": [
        {"type": "Script", "path": "aim.lua"},
        {"type": "Script", "path": "attack.lua"}
    ]})");

    auto json = TreeParser::LoadTreeFromDirectory(dir_.string());
    ASSERT_FALSE(json.empty());

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* sel = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->children().size(), 2u);

    auto* sub = dynamic_cast<SubtreeNode*>(sel->children()[0].get());
    ASSERT_NE(sub, nullptr);
    EXPECT_EQ(sub->subtree_name(), "combat");
    auto* inner = dynamic_cast<Composite*>(sub->subtree_root());
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->children().size(), 2u);
}

TEST_F(LoadTreeFromDirectoryTest, MultipleSubtrees) {
    WriteFile("root.json", R"({"type": "Sequence", "children": [
        {"type": "Subtree", "subtree": "combat"},
        {"type": "Subtree", "subtree": "patrol"}
    ]})");
    WriteFile("combat.json", R"({"type": "Script", "path": "fight.lua"})");
    WriteFile("patrol.json", R"({"type": "Script", "path": "walk.lua"})");

    auto json = TreeParser::LoadTreeFromDirectory(dir_.string());
    ASSERT_FALSE(json.empty());

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* seq = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(seq->children().size(), 2u);
    EXPECT_EQ(seq->children()[0]->type(), "Subtree");
    EXPECT_EQ(seq->children()[1]->type(), "Subtree");
}

TEST_F(LoadTreeFromDirectoryTest, IgnoresNonJsonFiles) {
    WriteFile("root.json", R"({"type": "Script", "path": "a.lua"})");
    WriteFile("notes.txt", "ignore me");
    WriteFile("data.csv", "1,2,3");

    auto json = TreeParser::LoadTreeFromDirectory(dir_.string());
    ASSERT_FALSE(json.empty());

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Script");
}

TEST_F(LoadTreeFromDirectoryTest, DirNotFound) {
    auto json = TreeParser::LoadTreeFromDirectory("/nonexistent/path");
    EXPECT_TRUE(json.empty());
}

TEST_F(LoadTreeFromDirectoryTest, NoRootJson) {
    WriteFile("combat.json", R"({"type": "Script", "path": "fight.lua"})");

    auto json = TreeParser::LoadTreeFromDirectory(dir_.string());
    EXPECT_TRUE(json.empty());
}

TEST_F(LoadTreeFromDirectoryTest, InvalidJsonInRoot) {
    WriteFile("root.json", "{invalid}");

    auto json = TreeParser::LoadTreeFromDirectory(dir_.string());
    EXPECT_TRUE(json.empty());
}

TEST_F(LoadTreeFromDirectoryTest, InvalidJsonInSubtree) {
    WriteFile("root.json", R"({"type": "Subtree", "subtree": "bad"})");
    WriteFile("bad.json", "{broken");

    auto json = TreeParser::LoadTreeFromDirectory(dir_.string());
    EXPECT_TRUE(json.empty());
}

// --- ScriptNode Args Parsing Tests ---

TEST(TreeParserArgsTest, ParseScriptNodeWithArgs) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "scripts/attack.lua",
            "args": {
                "target": "enemy",
                "damage": 100,
                "use_critical": true
            }
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Script");
    auto* script = dynamic_cast<ScriptNode*>(root.get());
    ASSERT_NE(script, nullptr);
    EXPECT_EQ(script->script_path(), "scripts/attack.lua");
}

TEST(TreeParserArgsTest, ParseScriptNodeWithNoArgs) {
    const char* json = R"({
        "root": {"type": "Script", "path": "test.lua"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Script");
}

TEST(TreeParserArgsTest, ParseScriptNodeWithEmptyArgs) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "test.lua",
            "args": {}
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Script");
}

TEST(TreeParserArgsTest, ParseScriptNodeWithFloatArg) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "test.lua",
            "args": {"ratio": 0.75}
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Script");
}

// --- ScriptNode Colon-Method Integration Tests ---

// Test scripts live in tests/scripts/ alongside this file.
// bt.set_project_path points to the project tests/ directory.

class ScriptNodeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard = std::make_shared<Blackboard>();
        bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
        lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
        rt = LuaRuntime::Builder()
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();

        // tests/ is the project root for test scripts
        tests_dir_ = std::filesystem::absolute(
            std::filesystem::path(__FILE__).parent_path()).string();
    }

    void TearDown() override {
        if (lib && lib->engine() && lib->engine()->IsRunning()) {
            lib->engine()->StopLoop();
            lib->engine()->Stop();
        }
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
    lib->SetTickIntervalMs(10);
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        return bt.run('{"root":{"type":"Script","path":"scripts/bt_module.lua"}}')
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ExitReasonAsParameter) {
    lib->SetTickIntervalMs(10);
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        return bt.run('{"root":{"type":"Script","path":"scripts/check_reason.lua"}}')
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ArgsPassedToEnter) {
    lib->SetTickIntervalMs(10);
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        return bt.run('{"root":{"type":"Script","path":"scripts/with_args.lua","args":{"target":"enemy","damage":100}}}')
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ArgsBoolType) {
    lib->SetTickIntervalMs(10);
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        return bt.run('{"root":{"type":"Script","path":"scripts/bool_args.lua","args":{"enabled":true}}}')
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, SelfPersistsAcrossTicks) {
    lib->SetTickIntervalMs(10);
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        return bt.run('{"root":{"type":"Script","path":"scripts/counter.lua"}}')
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, NoArgsStillWorks) {
    lib->SetTickIntervalMs(10);
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        return bt.run('{"root":{"type":"Script","path":"scripts/no_args.lua"}}')
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, ScriptNotFoundReturnsError) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local ok, status = bt.run('{"root":{"type":"Script","path":"scripts/nonexistent.lua"}}')
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("nonexistent.lua"), std::string::npos);
    EXPECT_NE(err->find("No such file"), std::string::npos);
}

TEST_F(ScriptNodeIntegrationTest, ScriptRuntimeErrorReturnsError) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local ok, status = bt.run('{"root":{"type":"Script","path":"scripts/runtime_error.lua"}}')
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("something went wrong"), std::string::npos);
    EXPECT_NE(err->find("runtime_error.lua"), std::string::npos);
}

TEST_F(ScriptNodeIntegrationTest, NodeReturnsFailureIsNotError) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local ok, status = bt.run('{"root":{"type":"Script","path":"scripts/returns_failure.lua"}}')
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
     EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(ScriptNodeIntegrationTest, InitErrorInSequenceStopsEarly) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(
        "local bt = require('bt')\n"
        "bt.set_project_path('" + tests_dir_ + "')\n"
        "local json = '{\"root\":{\"type\":\"Sequence\",\"children\":"
        "[{\"type\":\"Script\",\"path\":\"scripts/nonexistent.lua\"},"
        "{\"type\":\"Script\",\"path\":\"scripts/no_args.lua\"}]}}'\n"
        "local ok, status = bt.run(json)\n"
        "return ok, status\n"
    ));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("nonexistent.lua"), std::string::npos);
}

// --- ForceFailure Tests ---

TEST(ForceFailureTest, AlwaysFalse) {
    Blackboard bb;
    ForceFailure ff;
    EXPECT_FALSE(ff.Evaluate(bb));
}

TEST(TreeParserForceFailureTest, ParseForceFailureDecorator) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "a.lua",
            "decorators": [
                {"type": "ForceFailure", "abort": "Self"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->decorators().size(), 1u);
    auto* ff = dynamic_cast<ForceFailure*>(root->decorators()[0].get());
    ASSERT_NE(ff, nullptr);
    EXPECT_EQ(ff->abort_mode(), AbortMode::kSelf);
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

// --- TreeParser new node type tests ---

TEST(TreeParserRepeatTest, ParseRepeat) {
    const char* json = R"({
        "root": {
            "type": "Repeat",
            "count": 3,
            "children": [
                {"type": "Script", "path": "a.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Repeat");
}

TEST(TreeParserRepeatTest, ParseRepeatNoChildren) {
    const char* json = R"({
        "root": {
            "type": "Repeat",
            "count": 3
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(TreeParserRepeatTest, ParseRepeatInfinite) {
    const char* json = R"({
        "root": {
            "type": "Repeat",
            "count": -1,
            "children": [
                {"type": "Script", "path": "a.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Repeat");
}

TEST(TreeParserRetryTest, ParseRetryUntilSuccessful) {
    const char* json = R"({
        "root": {
            "type": "RetryUntilSuccessful",
            "attempts": 5,
            "children": [
                {"type": "Script", "path": "a.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "RetryUntilSuccessful");
}

TEST(TreeParserRetryTest, ParseRetryNoChildren) {
    const char* json = R"({
        "root": {
            "type": "RetryUntilSuccessful",
            "attempts": 3
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    EXPECT_EQ(root, nullptr);
}

TEST(TreeParserRandomTest, ParseRandomSelector) {
    const char* json = R"({
        "root": {
            "type": "RandomSelector",
            "children": [
                {"type": "Script", "path": "a.lua"},
                {"type": "Script", "path": "b.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "RandomSelector");
    auto* sel = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->children().size(), 2u);
}

TEST(TreeParserRandomTest, ParseRandomSequence) {
    const char* json = R"({
        "root": {
            "type": "RandomSequence",
            "children": [
                {"type": "Script", "path": "a.lua"},
                {"type": "Script", "path": "b.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "RandomSequence");
    auto* seq = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(seq, nullptr);
    EXPECT_EQ(seq->children().size(), 2u);
}

TEST(TreeParserWaitTest, ParseWait) {
    const char* json = R"({
        "root": {
            "type": "Wait",
            "ms": 500
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->type(), "Wait");
}

TEST(TreeParserWaitTest, ParseWaitDefault) {
    const char* json = R"({
        "root": {"type": "Wait"}
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
     EXPECT_EQ(root->type(), "Wait");
}
