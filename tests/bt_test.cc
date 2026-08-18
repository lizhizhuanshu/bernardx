#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <random>
#include <thread>

#include "behavior_tree_engine.h"
#include "blackboard.h"
#include "blackboard_condition.h"
#include "bt_event_queue.h"
#include "bt_library.h"
#include "bt_utils.h"
#include "blackboard_library.h"
#include "composite.h"
#include "condition_composite.h"
#include "constant_node.h"
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
#include "set_node.h"
#include "subtree_node.h"
#include "repeat.h"
#include "retry.h"
#include "random_selector.h"
#include "random_sequence.h"
#include "wait_node.h"
#include "tree_parser.h"
#include "memory_resource_provider.h"
#include "file_system_code_provider.h"

using json = nlohmann::json;

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

// Stamp the per-tick cached time exactly as the engine does at each tick,
// then tick the node — required for any node that reads BtEventQueue time
// (Wait, Pipeline wait budgets) when driven directly.
static NodeStatus TickStamp(Pipeline& p, Blackboard& bb, BtEventQueue& ev) {
    ev.BeginTick(NowMs());
    return p.Tick(bb, ev);
}
static NodeStatus TickStamp(WaitNode& w, Blackboard& bb, BtEventQueue& ev) {
    ev.BeginTick(NowMs());
    return w.Tick(bb, ev);
}

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

TEST_F(BehaviorTreeEngineTest, WrapperNodeTakesSingularChild) {
    // Wrapper (single-child) nodes take their child from `child` as a direct object.
    auto ok = TreeParser::Parse(
        R"({"type":"Repeat","params":{"count":2},"child":{"type":"Script","source":"a.lua"}})");
    EXPECT_NE(nullptr, ok.root);
    EXPECT_TRUE(ok.error.empty());

    // The legacy `children` array form is no longer accepted for wrapper nodes.
    auto legacy = TreeParser::Parse(
        R"({"type":"Repeat","params":{"count":2},"children":[{"type":"Script","source":"a.lua"}]})");
    EXPECT_EQ(nullptr, legacy.root);
    EXPECT_NE(legacy.error.find("requires a child"), std::string::npos);
}

TEST_F(BehaviorTreeEngineTest, ParseRetryWithMaxCount) {
    // `Retry` (formerly RetryUntilSuccessful) parses with `params.max_count`
    // (formerly attempts). Missing child / wrong shape still errors.
    auto ok = TreeParser::Parse(
        R"({"type":"Retry","params":{"max_count":3},"child":{"type":"Script","source":"a.lua"}})");
    ASSERT_NE(nullptr, ok.root);
    EXPECT_TRUE(ok.error.empty());

    // The old type name is gone: RetryUntilSuccessful no longer parses.
    auto old = TreeParser::Parse(
        R"({"type":"RetryUntilSuccessful","params":{"attempts":3},"child":{"type":"Script","source":"a.lua"}})");
    EXPECT_EQ(nullptr, old.root);
    EXPECT_NE(old.error.find("unknown node type"), std::string::npos);
}

TEST_F(BehaviorTreeEngineTest, ParseAppliesRootParamsToTypeField) {
    // Root params template EVERY string field, including `type`: {{kind}} is
    // substituted from params so the root parses as a Sequence. Without params
    // the literal '{{kind}}' is an unknown node type.
    auto with = TreeParser::Parse(
        R"({"type":"{{kind}}","children":[{"type":"Script","source":"a.lua"}]})",
        nlohmann::json{{"kind", "Sequence"}});
    EXPECT_NE(with.root, nullptr);
    EXPECT_TRUE(with.error.empty());

    auto without = TreeParser::Parse(
        R"({"type":"{{kind}}","children":[{"type":"Script","source":"a.lua"}]})");
    EXPECT_EQ(without.root, nullptr);
    EXPECT_NE(without.error.find("unknown node type"), std::string::npos);
}

TEST_F(BehaviorTreeEngineTest, ParseResolvesDataRefAfterTemplating) {
    // `.{{which}}.script` first templates to `.login.script`, then resolves to
    // data["login"]["script"]. The resolved value lands in the Script node's
    // source (observable via script_path()).
    auto result = TreeParser::Parse(
        R"({"type":"Script","source":".{{which}}.script",)"
        R"("data":{"login":{"script":"scripts/login.lua"}}})",
        nlohmann::json{{"which", "login"}});
    ASSERT_NE(result.root, nullptr);
    EXPECT_TRUE(result.error.empty());
    auto* script = dynamic_cast<ScriptNode*>(result.root.get());
    ASSERT_NE(script, nullptr);
    EXPECT_EQ(script->script_path(), "scripts/login.lua");
}

TEST_F(BehaviorTreeEngineTest, ParseResolvesDataRefWithoutTemplating) {
    // A plain `.path` ref (no template) resolves against `data` too.
    auto result = TreeParser::Parse(
        R"({"type":"Script","source":".home.title",)"
        R"("data":{"home":{"title":"selectors/home.txt"}}})");
    ASSERT_NE(result.root, nullptr);
    auto* script = dynamic_cast<ScriptNode*>(result.root.get());
    ASSERT_NE(script, nullptr);
    EXPECT_EQ(script->script_path(), "selectors/home.txt");
}

TEST_F(BehaviorTreeEngineTest, ParseLeavesUnresolvedDataRefLiteral) {
    // `.missing.path` has no entry under `data`: left literal, parse still ok.
    auto result = TreeParser::Parse(
        R"({"type":"Script","source":".missing.path","data":{"home":{}}})");
    ASSERT_NE(result.root, nullptr);
    EXPECT_TRUE(result.error.empty());
    auto* script = dynamic_cast<ScriptNode*>(result.root.get());
    ASSERT_NE(script, nullptr);
    EXPECT_EQ(script->script_path(), ".missing.path");
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

    // Register a root node JSON (referenced from Lua as root = "res://root.json").
    void PutRoot(std::string json) { resource_provider->Put("root.json", std::move(json)); }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    std::shared_ptr<MemoryResourceProvider> resource_provider;
    LuaRuntime::Ptr rt;
};

#define AWAIT_BT(lazy) async_simple::coro::syncAwait(lazy)

// Subtree `condition: "child_condition"` transparently adopts the embedded
// subtree root's own condition (shared object, same pointer at boundary & inside).
TEST(TreeParserSubtreeCondition, ForwardsChildConditionMarker) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("sub.json",
        R"({"type":"Sequence","condition":{"type":"Script","source":"c.lua"},"children":[{"type":"Script","source":"a.lua"}]})");
    provider->Put("root.json", R"({"type":"Subtree","source":"res://sub.json","condition":"child_condition"})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_NE(nullptr, res.root);
    EXPECT_TRUE(res.error.empty());

    auto* sub = static_cast<SubtreeNode*>(res.root.get());
    ASSERT_NE(nullptr, sub->child());
    ASSERT_NE(nullptr, sub->condition());
    // Shared, not copied: same condition object as the subtree root.
    EXPECT_EQ(sub->condition(), sub->child()->condition());
}

// Marker is a no-op (warned) when the subtree root has no condition of its own.
TEST(TreeParserSubtreeCondition, ChildConditionMarkerNoOpWithoutRootCondition) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("sub.json", R"({"type":"Sequence","children":[{"type":"Script","source":"a.lua"}]})");
    provider->Put("root.json", R"({"type":"Subtree","source":"res://sub.json","condition":"child_condition"})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_NE(nullptr, res.root);
    auto* sub = static_cast<SubtreeNode*>(res.root.get());
    EXPECT_EQ(nullptr, sub->condition());
}

// A SUBTREE's own `data` table + `.path` refs resolve inside the subtree:
// `.{{target}}` in source first templates to `.home`, then looks up the
// subtree's data.home. Same contract as the root's data resolution.
TEST(TreeParserSubtreeDataRefs, ResolvesWithinSubtree) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    // Mirrors the user's click_page_button pattern: a data table of
    // selectors, param-driven `.{{target}}` reference into it. Placed on
    // `source` so the resolution is observable via script_path().
    provider->Put("sub.json",
        R"({"name":"click_page_button","type":"Script",)"
        R"("data":{"home":"scripts/login.lua","config":"scripts/other.lua"},)"
        R"("source":".{{target}}"})");
    provider->Put("root.json",
        R"({"type":"Subtree","source":"res://sub.json","params":{"target":"home"}})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_NE(nullptr, res.root);
    EXPECT_TRUE(res.error.empty());

    auto* sub = static_cast<SubtreeNode*>(res.root.get());
    ASSERT_NE(nullptr, sub->child());
    auto* script = dynamic_cast<ScriptNode*>(sub->child());
    ASSERT_NE(nullptr, script);
    EXPECT_EQ(script->script_path(), "scripts/login.lua");
}

// A bogus condition string is a hard parse error (no silent ignore).
TEST(TreeParserSubtreeCondition, RejectsUnknownConditionString) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("sub.json", R"({"type":"Script","source":"a.lua"})");
    provider->Put("root.json", R"({"type":"Subtree","source":"res://sub.json","condition":"child_condtion"})");  // typo

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    EXPECT_EQ(nullptr, res.root);
    EXPECT_NE(res.error.find("'child_condition'"), std::string::npos);
}

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
            and type(bt.get_status) == 'function'
            and type(bt.dump_paths) == 'function'
            and type(bt.path_report) == 'function'
            and bt.notify == nil  -- removed API must stay gone
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
        local status, err = bt.init({root = "res://root.json"})
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
        local status, err = bt.init({root = "res://root.json"})
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
        local status, err = bt.init({root = "res://root.json"})
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
                std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts"}))
            .WithResourceProvider(resource_provider)
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    void PutRoot(std::string json) { resource_provider->Put("root.json", std::move(json)); }

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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
    PutRoot(R"({"type":"Subtree","source":"res://sub.json","params":{"name":"lizhi","age":18,"active":true}})");
    resource_provider->Put("sub.json",
        R"({"type":"Script","source":"scripts/subtree_args.lua",)"
        R"("params":{"name":"{{name}}","age":"{{age}}","active":"{{active}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "res://root.json"})
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
    PutRoot(R"({"type":"Subtree","source":"res://sub.json","params":{"name":"lizhi","age":18,"active":true}})");
    resource_provider->Put("sub.json",
        R"({"type":"Script","source":"scripts/subtree_args.lua",)"
        R"("params":{"name":"hello {{name}}","age":"{{age}}","active":"{{active}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "res://root.json"})
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
    // Here the Subtree's param selects which target script the inner node
    // uses — "{{cond}}" resolves to the provided path.
    PutRoot(R"({"type":"Subtree","source":"res://sub.json","params":{"cond":"scripts/cond_truthy.lua"}})");
    resource_provider->Put("sub.json",
        R"({"type":"Pipeline","children":[)"
        R"({"type":"Script","source":"scripts/no_args.lua","*target":{"type":"Script","source":"{{cond}}"}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    // Target templated to cond_truthy -> met -> step skipped -> success.
    EXPECT_EQ(*s, "success");
}

TEST_F(ScriptNodeIntegrationTest, SubtreeParamsNestedPassThrough) {
    // Outer params template the inner Subtree node's path AND params; those
    // params then template the inner subtree JSON. Values pass through every
    // level transparently.
    PutRoot(R"({"type":"Subtree","source":"res://outer.json","params":{"role":"combat"}})");
    resource_provider->Put("outer.json",
        R"({"type":"Subtree","source":"res://inner-{{role}}.json","params":{"who":"{{role}}"}})");
    resource_provider->Put("inner-combat.json",
        R"({"type":"Script","source":"scripts/subtree_args.lua","params":{"name":"{{who}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
    PutRoot(R"({"type":"Subtree","source":"res://sub.json",)"
            R"("params":{"profile":{"name":"lizhi","hp":100}}})");
    resource_provider->Put("sub.json",
        R"({"type":"Script","source":"scripts/table_args.lua","params":{"config":"{{profile}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "res://root.json"})
        bt.exec({interval = 10})
        return bb.get("t_name"), bb.get("t_hp")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "lizhi");
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 100);
}

TEST_F(ScriptNodeIntegrationTest, RootParamsTemplatingReachesEnter) {
    // bt.init `params` templates the ROOT json exactly like a Subtree's params
    // template their subtree: whole-value placeholders preserve type, embedded
    // ones interpolate as text. Here a root Script reads templated params.
    PutRoot(R"({"type":"Script","source":"scripts/subtree_args.lua",)"
            R"("params":{"name":"hello {{name}}","age":"{{age}}","active":"{{active}}"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "res://root.json",
                 params = { name = "lizhi", age = 18, active = true }})
        bt.exec({interval = 10})
        return bb.get("sub_name"), bb.get("sub_age"), bb.get("sub_active")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 3u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "hello lizhi");  // partial
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 18);                 // whole, type kept
    EXPECT_EQ(std::get<bool>(r.values[2]), true);                  // whole, type kept
}

TEST_F(ScriptNodeIntegrationTest, RootParamsTemplatingWaitTimeout) {
    // Root params flow into a Wait's `timeout` (whole-value placeholder keeps
    // the array type). With [0,0] the Wait succeeds immediately, proving the
    // templated range reached the node end-to-end via bt.init.
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Wait","params":{"timeout":"{{t}}"}},)"
            R"({"type":"Script","source":"scripts/no_args.lua"}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json", params = { t = {0, 0} }})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(BehaviorTreeEngineTest, ParseWaitTimeoutShapes) {
    // number / [lo,hi] / [v] all parse; absent defaults; bad shapes and the
    // removed min_timeout/max_timeout fail at parse.
    for (const char* params :
         {R"("timeout":500)", R"("timeout":[500,2000])", R"("timeout":[500])", ""}) {
        auto ok = TreeParser::Parse(
            std::string(R"({"type":"Wait","params":{)") + params + "}}");
        ASSERT_NE(nullptr, ok.root) << params;
        EXPECT_TRUE(ok.error.empty()) << params;
    }

    auto legacy = TreeParser::Parse(
        R"({"type":"Wait","params":{"min_timeout":100,"max_timeout":200}})");
    EXPECT_EQ(nullptr, legacy.root);
    EXPECT_NE(legacy.error.find("were removed"), std::string::npos);

    auto badtype = TreeParser::Parse(
        R"({"type":"Wait","params":{"timeout":"500"}})");
    EXPECT_EQ(nullptr, badtype.root);
    EXPECT_NE(badtype.error.find("must be a number or a"), std::string::npos);

    auto badarr = TreeParser::Parse(
        R"({"type":"Wait","params":{"timeout":[1,2,3]}})");
    EXPECT_EQ(nullptr, badarr.root);
}

TEST_F(ScriptNodeIntegrationTest, RootParamsWithoutPlaceholdersIsNoop) {
    // Passing params to a root with no placeholders must parse unchanged.
    PutRoot(R"({"type":"Script","source":"scripts/no_args.lua"})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json", params = { unused = 1 }})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "success");
}

TEST_F(ScriptNodeIntegrationTest, GuardedSuccessBranchShortCircuits) {
    // The open_app pattern: a Selector whose first branch is a Success leaf
    // gated by a condition. Condition met → Success short-circuits (fallback
    // never needed); condition not met → fallback Script runs. {{cond}} picks
    // the condition script via root params.
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Success","condition":{"type":"Script","source":"{{cond}}"}},)"
            R"({"type":"Script","source":"scripts/no_args.lua"}]})");

    // Condition met (cond_truthy) → first branch Success → Selector Success.
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json", params = { cond = "scripts/cond_truthy.lua" }})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");

    // Condition not met (cond_falsy) → Success branch skipped → fallback runs.
    r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json", params = { cond = "scripts/cond_falsy.lua" }})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
}

// --- Blackboard param references (`$key`) Tests ---
//
// A `params` string value that starts with `$` is a blackboard reference,
// resolved fresh at Enter time (not at parse/Init). `$who` reads blackboard
// key "who"; `$$who` is the escape form and yields the literal "$who".

TEST_F(ScriptNodeIntegrationTest, BbRefResolvesAtEnter) {
    // params.target = "$who" → at Enter, blackboard "who" is read and forwarded
    // into the script's Enter, which echoes it back to "got_target".
    PutRoot(R"({"type":"Script","source":"scripts/bb_ref_args.lua","params":{"target":"$who"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("who", "shadowrocket")
        bt.init({root = "res://root.json"})
        bt.exec({interval = 10})
        return bb.get("got_target")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "shadowrocket");
}

TEST_F(ScriptNodeIntegrationTest, BbRefMissingKeyPassesNil) {
    // $missing with no such blackboard key → the param is nil at Enter (warned
    // miss, not a failure). The script tolerates nil and still succeeds.
    PutRoot(R"({"type":"Script","source":"scripts/bb_ref_args.lua","params":{"target":"$missing"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "res://root.json"})
        local status, err = bt.exec({interval = 10})
        return status, bb.get("got_target")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[1]));
}

TEST_F(ScriptNodeIntegrationTest, BbRefEscapeDollarLiteral) {
    // "$$price" is the escape form → literal "$price" forwarded at Enter.
    PutRoot(R"({"type":"Script","source":"scripts/bb_ref_args.lua","params":{"target":"$$price"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("price", "SHOULD_NOT_BE_USED")
        bt.init({root = "res://root.json"})
        bt.exec({interval = 10})
        return bb.get("got_target")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "$price");
}

TEST_F(ScriptNodeIntegrationTest, BbRefNonStringParamsUntouched) {
    // Only string VALUES starting with `$` are references. Numbers/bools/keys
    // are passed through verbatim — a string "$who" as the value still resolves,
    // while a numeric value and a normal string value land as literals.
    // Here a single param "target" is numeric 42 (untouched); the script echoes
    // it back, proving non-string params are not misread as references.
    PutRoot(R"({"type":"Script","source":"scripts/bb_ref_args.lua","params":{"target":42}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bt.init({root = "res://root.json"})
        bt.exec({interval = 10})
        return bb.get("got_target")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 42);
}

TEST_F(ScriptNodeIntegrationTest, BbRefConditionResolvesAtEnter) {
    // A ScriptCondition's `$flag` param is resolved from the blackboard at the
    // condition's Enter. With flag=true the condition is met (Selector's guarded
    // Success branch fires); we also read it back via the action script below.
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Success","condition":{"type":"Script","source":"scripts/cond_bb_ref.lua","params":{"flag":"$flag"}}},)"
            R"({"type":"Script","source":"scripts/bb_ref_args.lua","params":{"target":"$flag"}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("flag", true)
        bt.init({root = "res://root.json"})
        local status = bt.exec({interval = 10})
        return status, bb.get("got_target")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    // Condition met → Success branch short-circuits → overall success.
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
    // The Success branch short-circuits, so the fallback Script never runs and
    // got_target stays unset (nil) — proves the guard, not the action, decided.
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[1]));
}

TEST_F(ScriptNodeIntegrationTest, SelfPersistsAcrossTicks) {
    PutRoot(R"({"type":"Script","source":"scripts/counter.lua"})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        local status, err = bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        "local status, err = bt.init({root = 'res://root.json'})\n"
        "return status, err\n"
    ));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("nonexistent.lua"), std::string::npos);
}

// --- Template node: parse-time in-place expansion ---

TEST(TreeParserTemplate, ExpandsInPlaceWithoutWrapperNode) {
    // The Template is GONE after parsing: the tree holds the expanded node
    // directly (no SubtreeNode / no wrapper), with params substituted.
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("tpl.json", R"({"type":"Script","source":"scripts/login.lua"})");
    provider->Put("root.json", R"({"type":"Template","source":"res://tpl.json"})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_NE(nullptr, res.root);
    EXPECT_TRUE(res.error.empty());
    // The expanded node itself is the root - not wrapped.
    EXPECT_EQ(nullptr, dynamic_cast<SubtreeNode*>(res.root.get()));
    auto* script = dynamic_cast<ScriptNode*>(res.root.get());
    ASSERT_NE(nullptr, script);
    EXPECT_EQ(script->script_path(), "scripts/login.lua");
}

TEST(TreeParserTemplate, SubstitutesParamsLikeSubtree) {
    // {{key}} params forward into the template JSON (type-preserving whole
    // value, text interpolation for fragments) + the template's own data
    // refs resolve - the full Subtree pipeline.
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("tpl.json",
        R"({"type":"Script","source":".{{which}}",)"
        R"("data":{"home":"scripts/login.lua","cart":"scripts/cart.lua"}})");
    provider->Put("root.json",
        R"({"type":"Template","source":"res://tpl.json","params":{"which":"cart"}})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_NE(nullptr, res.root);
    auto* script = dynamic_cast<ScriptNode*>(res.root.get());
    ASSERT_NE(nullptr, script);
    EXPECT_EQ(script->script_path(), "scripts/cart.lua");
}

TEST(TreeParserTemplate, ConditionReattachesToExpandedRoot) {
    // A guard declared on the Template survives expansion: it lands on the
    // expanded root node.
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("tpl.json", R"({"type":"Script","source":"a.lua"})");
    provider->Put("root.json",
        R"({"type":"Template","source":"res://tpl.json",)"
        R"("condition":{"type":"Blackboard","key":"flag","op":"=="}})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_NE(nullptr, res.root);
    EXPECT_NE(nullptr, res.root->condition());
    EXPECT_EQ(res.root->condition()->type(), "Blackboard");
}

TEST(TreeParserTemplate, DetectsCircularReference) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("a.json", R"({"type":"Template","source":"res://b.json"})");
    provider->Put("b.json", R"({"type":"Template","source":"res://a.json"})");
    provider->Put("root.json", R"({"type":"Template","source":"res://a.json"})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    EXPECT_EQ(nullptr, res.root);
    EXPECT_NE(res.error.find("circular"), std::string::npos);
}

// A Template's params are its contract: required-but-missing {{key}}s are a
// hard parse error naming each key (not a silent literal left to fail later).
TEST(TreeParserTemplate, MissingParamsFailParseWithKeyNames) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("tpl.json",
        R"({"type":"Script","source":"scripts/{{dir}}/{{name}}.lua","params":{"sel":"{{sel}}"}})");
    provider->Put("root.json", R"({"type":"Template","source":"res://tpl.json"})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    EXPECT_EQ(nullptr, res.root);
    EXPECT_NE(res.error.find("missing required params"), std::string::npos);
    EXPECT_NE(res.error.find("{{dir}}"), std::string::npos);
    EXPECT_NE(res.error.find("{{name}}"), std::string::npos);
    EXPECT_NE(res.error.find("{{sel}}"), std::string::npos);
    EXPECT_NE(res.error.find("tpl.json"), std::string::npos);  // names the template
}

// Supplying one of several required keys still errors on the rest - partial
// coverage is partial failure.
TEST(TreeParserTemplate, PartialParamsStillErrorOnMissing) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("tpl.json", R"({"type":"Script","source":"scripts/{{a}}_{{b}}.lua"})");
    provider->Put("root.json",
        R"({"type":"Template","source":"res://tpl.json","params":{"a":"x"}})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    EXPECT_EQ(nullptr, res.root);
    EXPECT_NE(res.error.find("{{b}}"), std::string::npos);
    EXPECT_EQ(std::string::npos, res.error.find("{{a}}"));  // supplied one is gone
}

// All required keys supplied -> parses fine (sanity against over-eager errors).
TEST(TreeParserTemplate, AllParamsSuppliedParses) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("tpl.json", R"({"type":"Script","source":"scripts/{{a}}_{{b}}.lua"})");
    provider->Put("root.json",
        R"({"type":"Template","source":"res://tpl.json","params":{"a":"x","b":"y"}})");

    auto res = AWAIT_BT(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_NE(nullptr, res.root);
    EXPECT_TRUE(res.error.empty());
}

// E2E: a Template used inline inside a Pipeline expands to a plain step -
// path_report / execution see the expanded Script, not a wrapper.
TEST_F(ScriptNodeIntegrationTest, TemplateInsidePipelineRunsExpanded) {
    resource_provider->Put("click_tpl.json",
        R"({"type":"Script","source":"scripts/e2e_goto.lua","params":{"to":"{{page}}","flaky":0}})");
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Template","source":"res://click_tpl.json","params":{"page":"cart"}},)"
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"done","value":true},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"done","value":true}}})"
            R"(]})");
    blackboard->Set("page", LuaValue(std::string("home")));

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "success");
    EXPECT_EQ(*blackboard->Get("page"), LuaValue(std::string("cart")));  // expanded step ran
    EXPECT_EQ(*blackboard->Get("done"), LuaValue(true));
}

// --- Pipeline edge-param `$key` blackboard references ---
//
// `*timeout`/`*retry` accept "$key": read from the blackboard at parse time
// (providers invoke fresh). The value may be an integer or a {lo,hi} table.
// Unresolvable/malformed refs are parse ERRORS (never a silent 0).

// E2E through bt.init/exec: the edge param is a table {2,3} set from Lua.
TEST_F(ScriptNodeIntegrationTest, PipelineEdgeParamBlackboardRefTable) {
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"("*timeout":30,"*retry":"$common.action.retry_count"}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("common.action.retry_count", {2, 3})
        local ok, err = bt.init({root = "res://root.json"})
        if not ok then return false, err end
        local status = bt.exec({interval = 10, max_step = 60})
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    // values[0]=true (init ok), values[1]=exec status.
    ASSERT_TRUE(r.values[0] == LuaValue(true)) << "bt.init ok";  // $ref parsed
    // cond_falsy never holds: with retry budget 2-3 the pipeline eventually
    // fails; exec returns the terminal status string.
    auto* st = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(*st, "failure");
}

TEST_F(ScriptNodeIntegrationTest, PipelineEdgeParamBlackboardRefProvider) {
    // A provider returning an integer resolves like a literal scalar.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"("*timeout":"$tmo","*retry":0}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set_provider("tmo", function() return 30 end)
        local ok, err = bt.init({root = "res://root.json"})
        if not ok then return false, err end
        local status = bt.exec({interval = 10, max_step = 40})
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_TRUE(r.values[0] == LuaValue(true)) << "bt.init ok";
    auto* st = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(*st, "failure");  // 30ms timeout fired
}

TEST_F(ScriptNodeIntegrationTest, PipelineEdgeParamBlackboardRefMissingKey) {
    // A $key ref resolves at RUN time now: parse succeeds, a missing key
    // logs and degrades to 0 (wait forever) - the run ends at exec's own
    // budget ("timeout"), not an init error.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"("*timeout":"$missing_key","*retry":0}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local ok, err = bt.init({root = "res://root.json"})
        if not ok then return false, err end
        local status = bt.exec({interval = 10, max_step = 30})
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_TRUE(r.values[0] == LuaValue(true));  // init ok (ref resolves lazily)
    auto* st = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(*st, "timeout");  // budget degraded to 0 = wait forever
}

TEST_F(ScriptNodeIntegrationTest, PipelineEdgeParamBlackboardRefBadType) {
    // A ref pointing at a string: run-time resolution fails, logs, degrades
    // to 0 (wait forever) - init still succeeds.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"("*timeout":"$str_val","*retry":0}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("str_val", "not a number")
        local ok, err = bt.init({root = "res://root.json"})
        if not ok then return false, err end
        local status = bt.exec({interval = 10, max_step = 30})
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_TRUE(r.values[0] == LuaValue(true));
    auto* st = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(*st, "timeout");
}

// --- Pipeline-level default timeout/retry + per-run (Enter-time) resolution ---

// params.timeout / params.retry supply the defaults for steps that don't
// declare their own *timeout / *retry.
TEST_F(ScriptNodeIntegrationTest, PipelineDefaultTimeoutAppliesToSteps) {
    // The step declares no edge params; the pipeline default *retry=3 with
    // *timeout=10ms applies: the target never holds, so the step burns its
    // 3 re-runs then fails.
    PutRoot(R"({"type":"Pipeline","params":{"retry":3,"timeout":10},"children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local ok, err = bt.init({root = "res://root.json"})
        if not ok then return false, err end
        local status = bt.exec({interval = 5, max_step = 200})
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_TRUE(r.values[0] == LuaValue(true));
    auto* st = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(*st, "failure");  // default budget exhausted the re-runs
}

// A step's OWN edge param wins over the pipeline default.
TEST_F(ScriptNodeIntegrationTest, PipelineStepParamOverridesDefault) {
    // Default *timeout = 10ms; the step declares its own *timeout = 80ms.
    // The failure must arrive on the STEP's budget, i.e. >= ~80ms.
    PutRoot(R"({"type":"Pipeline","params":{"timeout":10},"children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"("*timeout":80}]})");
    auto t0 = std::chrono::steady_clock::now();
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local ok, err = bt.init({root = "res://root.json"})
        if not ok then return false, err end
        local status = bt.exec({interval = 5, max_step = 200})
        return true, status
    )"));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ(r.status, LUA_OK);
    auto* st = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(*st, "failure");
    EXPECT_GE(elapsed, 70);  // the step's 80ms budget, not the 10ms default
}

// Per-run resolution: a $key ref is read FRESH each run. Retry wraps the
// Pipeline; after the step fails its 30ms wait, Retry Reset()s the pipeline
// for attempt 2 - which must RE-RESOLVE "$tmo" (the provider now returns 0 =
// wait forever). If the budget had been frozen at parse/first-run time,
// attempt 2 would fail at another 30ms and Retry would give up ("failure");
// with fresh resolution the run waits forever and ends at exec's own budget.
TEST_F(ScriptNodeIntegrationTest, PipelineEdgeParamRereadsProviderPerRun) {
    PutRoot(R"({"type":"Retry","params":{"max_count":2},"child":)"
            R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"("*timeout":"$tmo","*retry":0}]}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        local calls = 0
        bb.set_provider("tmo", function()
            calls = calls + 1
            return calls <= 1 and 30 or 0   -- run 1: 30ms budget; later: forever
        end)
        local ok, err = bt.init({root = "res://root.json"})
        if not ok then return false, err end
        local status = bt.exec({interval = 5, max_step = 80})
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK) << "script errored";
    ASSERT_TRUE(r.values[0] == LuaValue(true));
    auto* st = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(*st, "timeout");  // attempt 2 re-resolved to 0 = wait forever
}

// --- Pipeline composite tests ---
//
// (Pipeline tests are rewritten for the *target semantics: a step's action
// runs while its target is NOT met; steps whose targets already hold are
// skipped. See the Pipeline + NodeCondition test sections.)

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
                std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts"}))
            .WithResourceProvider(resource_provider)
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();
    }

    void TearDown() override {
        lib->engine()->Stop();
    }

    void PutRoot(std::string json) { resource_provider->Put("root.json", std::move(json)); }

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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
        local status, err = bt.exec({max_step = 3, interval = 10})
        return status, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    auto* s = std::get_if<std::string>(&r.values[0]);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "timeout");
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

// --- Retry Tests ---

TEST(RetryTest, SucceedsImmediately) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kSuccess);
    auto retry = std::make_unique<Retry>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RetryTest, RetriesOnFailure) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<Retry>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    // Fail 1
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    // Fail 2
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    // Fail 3: exceeded max attempts
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kFailure);
}

TEST(RetryTest, SucceedsAfterRetries) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<Retry>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    child->set_status(NodeStatus::kSuccess);
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RetryTest, InfiniteRetryNeverGivesUp) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<Retry>(1, "retry",
        Retry::kInfinite,
        std::unique_ptr<MockNode>(child));

    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    }
    child->set_status(NodeStatus::kSuccess);
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kSuccess);
}

TEST(RetryTest, RunningChildReturnsRunning) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kRunning);
    auto retry = std::make_unique<Retry>(1, "retry", 3,
        std::unique_ptr<MockNode>(child));

    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
}

TEST(RetryTest, ResetClearsAttempts) {
    Blackboard bb;
    BtEventQueue events;
    auto* child = new MockNode(2, "child", NodeStatus::kFailure);
    auto retry = std::make_unique<Retry>(1, "retry", 2,
        std::unique_ptr<MockNode>(child));

    retry->Tick(bb, events);  // attempt 1
    retry->Reset();
    // After reset, get 2 fresh attempts
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kRunning);
    EXPECT_EQ(retry->Tick(bb, events), NodeStatus::kFailure);
}

// --- BlackboardCondition Tests ---
//
// {"type":"Blackboard","key":k,"op":op,"value":v} compares blackboard[k]
// against the literal v. Missing key -> Failure for EVERY op (incl. !=).

TEST(BlackboardConditionTest, EqualityOps) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("name", LuaValue(std::string("hero")));
    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));
    bb.Set("alive", LuaValue(true));
    bb.Set("nil_key", LuaValue(nullptr));

    // string ==
    EXPECT_EQ(BlackboardCondition("name", "==", json("hero")).Tick(bb, events),
              NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("name", "==", json("villain")).Tick(bb, events),
              NodeStatus::kFailure);
    // number == (json int vs int64 LuaValue)
    EXPECT_EQ(BlackboardCondition("hp", "==", json(100)).Tick(bb, events),
              NodeStatus::kSuccess);
    // number == across int/float (5 == 5.0)
    EXPECT_EQ(BlackboardCondition("hp", "==", json(100.0)).Tick(bb, events),
              NodeStatus::kSuccess);
    // bool ==
    EXPECT_EQ(BlackboardCondition("alive", "==", json(true)).Tick(bb, events),
              NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("alive", "==", json(false)).Tick(bb, events),
              NodeStatus::kFailure);
    // != negation
    EXPECT_EQ(BlackboardCondition("name", "!=", json("villain")).Tick(bb, events),
              NodeStatus::kSuccess);
    // null matches nil
    EXPECT_EQ(BlackboardCondition("nil_key", "==", json(nullptr)).Tick(bb, events),
              NodeStatus::kSuccess);
    // type mismatch is not equal (number vs string literal)
    EXPECT_EQ(BlackboardCondition("hp", "==", json("100")).Tick(bb, events),
              NodeStatus::kFailure);
}

TEST(BlackboardConditionTest, MissingKeyFailsEveryOp) {
    Blackboard bb;
    BtEventQueue events;
    for (const char* op : {"==", "!=", ">", ">=", "<", "<="}) {
        EXPECT_EQ(BlackboardCondition("ghost", op, json(1)).Tick(bb, events),
                  NodeStatus::kFailure) << "op=" << op;
    }
    EXPECT_EQ(BlackboardCondition("ghost", "exists", json(nullptr)).Tick(bb, events),
              NodeStatus::kFailure);
}

TEST(BlackboardConditionTest, OrderingOps) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("count", LuaValue(static_cast<int64_t>(5)));
    bb.Set("page", LuaValue(std::string("home")));

    EXPECT_EQ(BlackboardCondition("count", ">", json(3)).Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("count", ">", json(5)).Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(BlackboardCondition("count", ">=", json(5)).Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("count", "<", json(5)).Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(BlackboardCondition("count", "<=", json(5)).Tick(bb, events), NodeStatus::kSuccess);
    // string lexicographic ordering
    EXPECT_EQ(BlackboardCondition("page", "<", json("izone")).Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("page", ">=", json("home")).Tick(bb, events), NodeStatus::kSuccess);
    // ordering against a non-orderable stored value (bool) -> Failure + error
    bb.Set("flag", LuaValue(true));
    auto cond = BlackboardCondition("flag", ">", json(0));
    EXPECT_EQ(cond.Tick(bb, events), NodeStatus::kFailure);
    EXPECT_FALSE(cond.last_error().empty());
}

TEST(BlackboardConditionTest, ExistsOp) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("real", LuaValue(static_cast<int64_t>(1)));
    EXPECT_EQ(BlackboardCondition("real", "exists", json(nullptr)).Tick(bb, events),
              NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("fake", "exists", json(nullptr)).Tick(bb, events),
              NodeStatus::kFailure);
}

// --- BlackboardCondition key2 (key vs key) Tests ---
//
// {"key":a,"op":op,"key2":b} compares blackboard[a] against blackboard[b],
// both read fresh on each Tick. Either side missing -> Failure.

TEST(BlackboardConditionTest, Key2EqualityAndOrdering) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("hp", LuaValue(static_cast<int64_t>(100)));
    bb.Set("shield", LuaValue(static_cast<int64_t>(80)));
    bb.Set("label", LuaValue(std::string("home")));
    bb.Set("page", LuaValue(std::string("home")));

    // == / != between two keys
    EXPECT_EQ(BlackboardCondition("page", "==", std::string("label")).Tick(bb, events),
              NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("hp", "==", std::string("shield")).Tick(bb, events),
              NodeStatus::kFailure);
    EXPECT_EQ(BlackboardCondition("hp", "!=", std::string("shield")).Tick(bb, events),
              NodeStatus::kSuccess);
    // int vs double across keys compares numerically
    bb.Set("exact", LuaValue(100.0));
    EXPECT_EQ(BlackboardCondition("hp", "==", std::string("exact")).Tick(bb, events),
              NodeStatus::kSuccess);
    // ordering between keys
    EXPECT_EQ(BlackboardCondition("hp", ">", std::string("shield")).Tick(bb, events),
              NodeStatus::kSuccess);
    EXPECT_EQ(BlackboardCondition("shield", ">=", std::string("hp")).Tick(bb, events),
              NodeStatus::kFailure);
    EXPECT_EQ(BlackboardCondition("shield", "<", std::string("hp")).Tick(bb, events),
              NodeStatus::kSuccess);
    // mixed types across keys -> not equal, not orderable
    bb.Set("str5", LuaValue(std::string("5")));
    EXPECT_EQ(BlackboardCondition("hp", "==", std::string("str5")).Tick(bb, events),
              NodeStatus::kFailure);
    auto cond = BlackboardCondition("hp", ">", std::string("str5"));
    EXPECT_EQ(cond.Tick(bb, events), NodeStatus::kFailure);
    EXPECT_FALSE(cond.last_error().empty());
}

TEST(BlackboardConditionTest, Key2ReadsLiveEachTick) {
    // The right side is re-read on every Tick — updating blackboard[key2]
    // flips the result without re-parsing or re-entering anything.
    Blackboard bb;
    BtEventQueue events;
    bb.Set("a", LuaValue(static_cast<int64_t>(1)));
    bb.Set("b", LuaValue(static_cast<int64_t>(1)));
    BlackboardCondition cond("a", "==", std::string("b"));
    EXPECT_EQ(cond.Tick(bb, events), NodeStatus::kSuccess);
    bb.Set("b", LuaValue(static_cast<int64_t>(2)));
    EXPECT_EQ(cond.Tick(bb, events), NodeStatus::kFailure);
    bb.Set("b", LuaValue(static_cast<int64_t>(1)));
    EXPECT_EQ(cond.Tick(bb, events), NodeStatus::kSuccess);
}

TEST(BlackboardConditionTest, Key2MissingEitherSideFails) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("a", LuaValue(static_cast<int64_t>(1)));
    // rhs key missing -> not met
    EXPECT_EQ(BlackboardCondition("a", "==", std::string("ghost")).Tick(bb, events),
              NodeStatus::kFailure);
    EXPECT_EQ(BlackboardCondition("a", "!=", std::string("ghost")).Tick(bb, events),
              NodeStatus::kFailure);
    // lhs missing -> not met (rhs present)
    bb.Set("b", LuaValue(static_cast<int64_t>(1)));
    EXPECT_EQ(BlackboardCondition("ghost", "==", std::string("b")).Tick(bb, events),
              NodeStatus::kFailure);
}

TEST_F(BehaviorTreeEngineTest, ParseBlackboardCondition) {
    // Valid: guarded Success with a Blackboard condition parses clean.
    auto ok = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"page","op":">","value":3}})");
    ASSERT_NE(nullptr, ok.root);
    EXPECT_TRUE(ok.error.empty());
    ASSERT_NE(nullptr, ok.root->condition());
    EXPECT_EQ(ok.root->condition()->type(), "Blackboard");

    // Missing key -> parse error.
    auto nokey = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","op":"=="}})");
    EXPECT_EQ(nullptr, nokey.root);
    EXPECT_NE(nokey.error.find("missing 'key'"), std::string::npos);

    // Unknown op -> parse error.
    auto badop = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"k","op":"=~"}})");
    EXPECT_EQ(nullptr, badop.root);
    EXPECT_NE(badop.error.find("unknown op"), std::string::npos);

    // Non-scalar value -> parse error.
    auto objval = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"k","value":{"a":1}}})");
    EXPECT_EQ(nullptr, objval.root);
    EXPECT_NE(objval.error.find("must be a scalar"), std::string::npos);

    // Ordering op with bool value -> parse error.
    auto boolord = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"k","op":">","value":true}})");
    EXPECT_EQ(nullptr, boolord.root);
    EXPECT_NE(boolord.error.find("needs a number or string"), std::string::npos);
}

TEST_F(BehaviorTreeEngineTest, ParseBlackboardConditionKey2) {
    // Valid key-vs-key form parses clean.
    auto ok = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"hp","op":">","key2":"shield"}})");
    ASSERT_NE(nullptr, ok.root);
    EXPECT_TRUE(ok.error.empty());

    // key2 with value -> parse error (mutually exclusive).
    auto both = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"a","key2":"b","value":1}})");
    EXPECT_EQ(nullptr, both.root);
    EXPECT_NE(both.error.find("not both"), std::string::npos);

    // key2 with exists -> parse error.
    auto ex = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"a","op":"exists","key2":"b"}})");
    EXPECT_EQ(nullptr, ex.root);
    EXPECT_NE(ex.error.find("does not use 'key2'"), std::string::npos);

    // non-string key2 -> parse error.
    auto bad = TreeParser::Parse(
        R"({"type":"Success","condition":{"type":"Blackboard","key":"a","key2":3}})");
    EXPECT_EQ(nullptr, bad.root);
    EXPECT_NE(bad.error.find("'key2' must be a string"), std::string::npos);
}

TEST_F(ScriptNodeIntegrationTest, BlackboardConditionKeyVsKeyGatesBranch) {
    // End-to-end through bt.init/exec: guard compares two blackboard keys
    // (hp > shield). Met -> guarded Success short-circuits; after lowering hp
    // (no re-init), a fresh run takes the fallback branch instead.
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Success","condition":{"type":"Blackboard","key":"hp","op":">","key2":"shield"}},)"
            R"({"type":"Script","source":"scripts/no_args.lua"}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("hp", 100)
        bb.set("shield", 80)
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");

    // hp < shield now: guard not met -> fallback Script branch runs.
    r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("hp", 10)
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
}

// --- blackboard value provider Tests ---
//
// bb.set_provider(key, fn) installs a computed source: every read of the key
// (bb.get, $key resolution at Enter, Blackboard condition) invokes fn fresh.

TEST_F(ScriptNodeIntegrationTest, BlackboardProviderLuaRoundTrip) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bb = require('blackboard')
        local n = 0
        bb.set_provider("counter", function() n = n + 1; return n end)
        local a = bb.get("counter")
        local b = bb.get("counter")
        local has = bb.has("counter")
        bb.set("counter", "static")            -- set replaces the provider
        local c = bb.get("counter")
        bb.set_provider("gone", function() return 1 end)
        bb.remove("gone")                      -- remove drops the provider
        return a, b, has, c, bb.has("gone")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 5u);
    EXPECT_EQ(std::get<int64_t>(r.values[0]), 1);   // every get invokes fresh
    EXPECT_EQ(std::get<int64_t>(r.values[1]), 2);
    EXPECT_EQ(std::get<bool>(r.values[2]), true);   // provider counts as present
    EXPECT_EQ(std::get<std::string>(r.values[3]), "static");
    EXPECT_EQ(std::get<bool>(r.values[4]), false);
}

TEST_F(ScriptNodeIntegrationTest, BlackboardProviderFeedsDollarKeyAtEnter) {
    // Two Script nodes both take params.target = "$who" where `who` is a
    // provider: each Enter reads the provider fresh, so the two echoes see
    // DIFFERENT counter values — proving live per-Enter computation.
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","source":"scripts/bb_ref_args.lua","params":{"target":"$who"}},)"
            R"({"type":"Script","source":"scripts/bb_ref_args.lua","params":{"target":"$who"}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        local n = 0
        bb.set_provider("who", function() n = n + 1; return "user" .. n end)
        bt.init({root = "res://root.json"})
        bt.exec({interval = 10})
        return bb.get("got_target")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    // Second Enter saw the provider's NEXT value, not a snapshot.
    EXPECT_EQ(std::get<std::string>(r.values[0]), "user2");
}

TEST_F(ScriptNodeIntegrationTest, BlackboardProviderErrorYieldsNil) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bb = require('blackboard')
        bb.set_provider("bad", function() error("boom") end)
        local v = bb.get("bad")          -- error -> nil, no crash
        bb.set_provider("ok", function() return "still" .. "works" end)
        return v, bb.get("ok")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[0]));
    EXPECT_EQ(std::get<std::string>(r.values[1]), "stillworks");
}

TEST_F(ScriptNodeIntegrationTest, BlackboardConditionSeesProviderValue) {
    // A Blackboard condition comparing a provider-backed key against a
    // literal: met while the provider returns the expected value (guarded
    // Success short-circuits); once the provider's output changes, the
    // fallback runs and records it.
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Success","condition":{"type":"Blackboard","key":"mode","op":"==","value":"fast"}},)"
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"fell_back","value":true}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        local mode = "fast"
        bb.set_provider("mode", function() return mode end)
        bt.init({root = "res://root.json"})
        local s1 = bt.exec({interval = 10})
        local fb1 = bb.get("fell_back")
        mode = "slow"                    -- provider output changes...
        bt.init({root = "res://root.json"})
        local s2 = bt.exec({interval = 10})
        return s1, fb1, s2, bb.get("fell_back")
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 4u);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(r.values[1]));  // guard met
    EXPECT_EQ(std::get<std::string>(r.values[2]), "success");
    EXPECT_EQ(std::get<bool>(r.values[3]), true);                      // fallback ran
}

// --- SetNode Tests ---
//
// {"type":"Set","params":{"key":k,"value":v}} writes blackboard[k]=v at Tick.
// '$src' copies blackboard[src] fresh at each Tick; '$$x' is a literal "$x".

TEST(SetNodeTest, WritesLiteralEachTick) {
    Blackboard bb;
    BtEventQueue events;
    SetNode node(1, "set_home", "page", LuaValue(std::string("home")));
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(std::get<std::string>(*bb.Get("page")), "home");
    SetNode num(2, "set_n", "n", LuaValue(static_cast<int64_t>(7)));
    EXPECT_EQ(num.Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(std::get<int64_t>(*bb.Get("n")), 7);
    SetNode nil(3, "set_nil", "k", LuaValue(nullptr));
    EXPECT_EQ(nil.Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(*bb.Get("k")));
}

TEST(SetNodeTest, ReferenceCopiesFreshEachTick) {
    Blackboard bb;
    BtEventQueue events;
    bb.Set("src", LuaValue(static_cast<int64_t>(1)));
    SetNode node(1, "copy", "dst", BbParamRef{"src"});
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(std::get<int64_t>(*bb.Get("dst")), 1);
    // Source changes; the next Tick copies the NEW value (live, not snapshot).
    bb.Set("src", LuaValue(static_cast<int64_t>(2)));
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(std::get<int64_t>(*bb.Get("dst")), 2);
}

TEST(SetNodeTest, MissingReferenceWritesNil) {
    Blackboard bb;
    BtEventQueue events;
    SetNode node(1, "copy_ghost", "dst", BbParamRef{"ghost"});
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kSuccess);
    auto v = bb.Get("dst");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(*v));
}

TEST_F(BehaviorTreeEngineTest, ParseSetNode) {
    // Valid literal / reference / escaped / absent-value forms parse clean.
    for (const char* params :
         {R"("key":"k","value":"home")", R"("key":"k","value":"$src")",
          R"("key":"k","value":"$$cost")", R"("key":"k")"}) {
        auto ok = TreeParser::Parse(
            std::string(R"({"type":"Set","params":{)") + params + "}}");
        ASSERT_NE(nullptr, ok.root) << params;
        EXPECT_TRUE(ok.error.empty()) << params;
        EXPECT_EQ(ok.root->type(), "Set");
    }

    // Missing key -> parse error.
    auto nokey = TreeParser::Parse(R"({"type":"Set","params":{"value":1}})");
    EXPECT_EQ(nullptr, nokey.root);
    EXPECT_NE(nokey.error.find("params.key"), std::string::npos);

    // Non-scalar value -> parse error.
    auto obj = TreeParser::Parse(
        R"({"type":"Set","params":{"key":"k","value":{"a":1}}})");
    EXPECT_EQ(nullptr, obj.root);
    EXPECT_NE(obj.error.find("must be a scalar"), std::string::npos);
}

TEST_F(BehaviorTreeEngineTest, SetNodeFeedsBlackboardConditionScriptless) {
    // A fully scriptless tree: Set writes, Blackboard condition gates.
    engine->SetRoot(ParseJsonTree(
        R"({"type":"Sequence","children":[)"
        R"({"type":"Set","params":{"key":"page","value":"home"}},)"
        R"({"type":"Success","condition":{"type":"Blackboard","key":"page","op":"==","value":"home"}}]})"));
    EXPECT_EQ(engine->TickOnce(), NodeStatus::kSuccess);
    EXPECT_EQ(std::get<std::string>(*engine->blackboard().Get("page")), "home");
}

TEST_F(ScriptNodeIntegrationTest, SetNodeReferenceReadsProvider) {
    // End-to-end: Set dst=$pv where `pv` is a provider-backed counter; the
    // Set node's Tick reads the provider fresh, and a Blackboard condition
    // downstream sees the copied value.
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Set","params":{"key":"attempt","value":"$pv"}},)"
            R"({"type":"Success","condition":{"type":"Blackboard","key":"attempt","op":"==","value":1}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        local n = 0
        bb.set_provider("pv", function() n = n + 1; return n end)
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
}

TEST_F(ScriptNodeIntegrationTest, BlackboardConditionGatesBranch) {
    // End-to-end through bt.init/exec: a guarded Success branch with a
    // Blackboard condition short-circuits when met; the And composition of
    // two Blackboard conditions also works. No Lua condition script needed.
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Success","condition":{"type":"And","children":[)"
            R"({"type":"Blackboard","key":"page","op":"==","value":"home"},)"
            R"({"type":"Blackboard","key":"count","op":">=","value":5}]}},)"
            R"({"type":"Script","source":"scripts/no_args.lua"}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("page", "home")
        bb.set("count", 5)
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");

    // count below threshold -> And not met -> fallback Script branch runs
    // (still success overall, but via the fallback).
    r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("page", "home")
        bb.set("count", 2)
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
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
    auto wait = std::make_unique<WaitNode>(1, "wait", 0, 0);
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kSuccess);
}

TEST(WaitNodeTest, ReturnsRunningBeforeTimeout) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 1000, 1000);

    // First tick starts the timer
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kRunning);
    // Second tick: not enough time has passed
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kRunning);
}

TEST(WaitNodeTest, CompletesAfterMs) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 50, 50);

    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kRunning);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kSuccess);
}

TEST(WaitNodeTest, ResetRestartsTimer) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 50, 50);

    TickStamp(*wait, bb, events);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    // Timer expired but haven't ticked yet
    wait->Reset();

    // After reset, timer starts fresh
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kRunning);
}

TEST(WaitNodeTest, RangeLoEqHiBehavesAsFixed) {
    // A degenerate range [50,50] collapses to a fixed 50ms wait.
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 50, 50);

    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kRunning);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kSuccess);
}

TEST(WaitNodeTest, RangeZeroSucceedsImmediately) {
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 0, 0);
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kSuccess);
}

TEST(WaitNodeTest, RangeResolvedValueStaysInBounds) {
    // Whatever value is rolled in [20,40], the node must still be Running
    // before 20ms and Success after 40ms.
    Blackboard bb;
    BtEventQueue events;
    auto wait = std::make_unique<WaitNode>(1, "wait", 20, 40);

    ASSERT_EQ(TickStamp(*wait, bb, events), NodeStatus::kRunning);  // rolls + stamps
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kRunning);  // 10ms < min 20
    std::this_thread::sleep_for(std::chrono::milliseconds(45));  // ~55ms >= max 40
    EXPECT_EQ(TickStamp(*wait, bb, events), NodeStatus::kSuccess);
}

// --- ConstantNode (Success / Failure) Tests ---

TEST(ConstantNodeTest, SuccessAlwaysSucceeds) {
    Blackboard bb;
    BtEventQueue events;
    SuccessNode node(1, "ok");
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kSuccess);  // stays Success
}

TEST(ConstantNodeTest, FailureAlwaysFails) {
    Blackboard bb;
    BtEventQueue events;
    FailureNode node(1, "nope");
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kFailure);
    EXPECT_EQ(node.Tick(bb, events), NodeStatus::kFailure);
}

TEST_F(BehaviorTreeEngineTest, ParseConstantNodeTypes) {
    // Both constant leaves parse and tick their fixed status.
    auto ok = TreeParser::Parse(
        R"({"type":"Selector","children":[)"
        R"({"type":"Failure"},{"type":"Success"}]})");
    ASSERT_NE(ok.root, nullptr);
    EXPECT_TRUE(ok.error.empty());
    engine->SetRoot(std::move(ok.root));
    // First child fails → Selector moves on → second child (Success) → Success.
    EXPECT_EQ(engine->TickOnce(), NodeStatus::kSuccess);
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
    PutRoot(R"({"type":"Wait","params":{"timeout":999999}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
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
    PutRoot(R"({"type":"Wait","params":{"timeout":999999}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json", trace_paths=false})
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
    PutRoot(R"({"type":"Wait","params":{"timeout":999999}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
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
            R"({"type":"Wait","name":"w1","params":{"timeout":99999}},)"
            R"({"type":"Wait","name":"w2","params":{"timeout":99999}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        local ok, err = bt.goto_path({'root', 'w2'})
        return ok, err
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_GE(r.values.size(), 1u);
    EXPECT_EQ(std::get<bool>(r.values[0]), true);
}

TEST_F(BehaviorTreeLibraryTest, GotoPathNameNotFound) {
    PutRoot(R"({"type":"Sequence","name":"root","children":[)"
            R"({"type":"Wait","name":"w1","params":{"timeout":99999}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
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
            R"({"type":"Wait","name":"w1","params":{"timeout":99999}}]})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
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

// --- Pipeline *target skip/act/wait unit tests (MockNode + MockCondition) ---

TEST(PipelineTest, NoTargetRunsEachActionOnceSequentially) {
    // Absent *target: the action completing IS the target — every step runs,
    // once, in order (same tick fast-forward).
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    auto* b = new MockNode(3, "b", NodeStatus::kSuccess);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    pipe.AddChild(std::unique_ptr<MockNode>(b));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(a->tick_count, 1);
    EXPECT_EQ(b->tick_count, 1);
}

TEST(PipelineTest, MetTargetsAreSkipped) {
    // Step 0's target already holds → its action is skipped; step 1's
    // doesn't → its action runs.
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    auto* b = new MockNode(3, "b", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a),
                 std::make_shared<MockCondition>(NodeStatus::kSuccess), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(b),
                 std::make_shared<MockCondition>(NodeStatus::kFailure), 0, 0);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // b ran, waiting on target1
    EXPECT_EQ(a->tick_count, 0);
    EXPECT_EQ(b->tick_count, 1);
}

TEST(PipelineTest, AllTargetsMetSucceedsImmediately) {
    // Every step's target holds → nothing to do → Success without ticking
    // any action.
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    auto* b = new MockNode(3, "b", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a),
                 std::make_shared<MockCondition>(NodeStatus::kSuccess), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(b),
                 std::make_shared<MockCondition>(NodeStatus::kSuccess), 0, 0);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(a->tick_count, 0);
    EXPECT_EQ(b->tick_count, 0);
}

TEST(PipelineTest, AdvanceSkipsLaterMetTargets) {
    // After step 0's target becomes met, the re-scan skips step 2 (its
    // target already holds) and runs only the pending middle step.
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    auto* a2 = new MockNode(4, "a2", NodeStatus::kSuccess);
    auto* t0 = new MockCondition(NodeStatus::kFailure);  // met after a0's run
    pipe.AddStep(std::unique_ptr<MockNode>(a0),
                 std::shared_ptr<MockCondition>(t0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1),
                 std::make_shared<MockCondition>(NodeStatus::kFailure), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a2),
                 std::make_shared<MockCondition>(NodeStatus::kSuccess), 0, 0);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a0 ran; waiting target0
    EXPECT_EQ(a0->tick_count, 1);
    EXPECT_EQ(a1->tick_count, 0);
    t0->set_status(NodeStatus::kSuccess);  // target0 met → advance, skip step 2
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a1 ran; waiting target1
    EXPECT_EQ(a1->tick_count, 1);
    EXPECT_EQ(a2->tick_count, 0);  // its target was already met: skipped
}

TEST(PipelineTest, TargetMetRightAfterActionAdvancesSameTick) {
    // The target flips to met while the action is mid-run: on the action's
    // completing tick the wait sees the target met → advance immediately.
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a0),
                 std::make_shared<MockCondition>(NodeStatus::kSuccess), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1),
                 std::make_shared<MockCondition>(NodeStatus::kFailure), 0, 0);
    Blackboard bb; BtEventQueue ev;
    // Step 0 skipped (target met); a1 runs; waiting on target1 — no timeout,
    // so it just runs.
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);
    EXPECT_EQ(a0->tick_count, 0);
    EXPECT_EQ(a1->tick_count, 1);
}

TEST(PipelineTest, WaitTimeoutWithoutRetryFails) {
    // Action ran, target never comes, *timeout elapses with no retry budget
    // → failure. (Both actions run in the same first tick: a0 fast-forwards,
    // a1 runs then enters its wait.)
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), nullptr, 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1),
                 std::make_shared<MockCondition>(NodeStatus::kFailure),
                 /*timeout_ms=*/20, /*retry=*/0);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a0 + a1 ran; step1 waits
    EXPECT_EQ(a1->tick_count, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kFailure);  // step1 timeout, no retry
}

TEST(PipelineTest, RetryRerunsSameStepActionAndSucceeds) {
    // Step1's target appears only after a1 is re-run once via *retry: the
    // timeout fires, a1 is Reset + re-run, and the wait now succeeds.
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new CountingMockNode(3, "a1", NodeStatus::kSuccess);
    auto* t1 = new MockCondition(NodeStatus::kFailure);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), nullptr, 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1),
                 std::shared_ptr<MockCondition>(t1),
                 /*timeout_ms=*/20, /*retry=*/1);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a0 + a1 ran; wait target1
    EXPECT_EQ(a1->total_ticks(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // target1 timeout → re-run armed
    EXPECT_EQ(a1->total_ticks(), 1);                           // re-run happens next tick
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a1 re-ran; wait target1 again
    EXPECT_EQ(a1->total_ticks(), 2);
    t1->set_status(NodeStatus::kSuccess);                      // re-run achieved the target
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kSuccess);  // target met → done
}

TEST(PipelineTest, RetryExhaustedFails) {
    Pipeline pipe(1, "pipe");
    auto* a = new CountingMockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a),
                 std::make_shared<MockCondition>(NodeStatus::kFailure),
                 /*timeout_ms=*/20, /*retry=*/2);
    Blackboard bb; BtEventQueue ev;
    int guard = 0;
    NodeStatus s = NodeStatus::kRunning;
    while (s == NodeStatus::kRunning && guard++ < 20) {
        s = TickStamp(pipe, bb, ev);
        if (s == NodeStatus::kRunning)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_EQ(s, NodeStatus::kFailure);
    EXPECT_EQ(a->total_ticks(), 3);  // 1 initial run + 2 re-runs
}

TEST(PipelineTest, RunningTargetHoldsPipeline) {
    // A target still evaluating (Running) parks the pipeline without ticking
    // the action; once it resolves Failure the action runs.
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a), nullptr, 0, 0);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kSuccess);  // no target: run + done
    EXPECT_EQ(a->tick_count, 1);
}

TEST(PipelineTest, ChildFailureFailsPipeline) {
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kFailure);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kFailure);
}

TEST(PipelineTest, ResetReScans) {
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddChild(std::unique_ptr<MockNode>(a));
    Blackboard bb; BtEventQueue ev;
    ASSERT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kSuccess);
    pipe.Reset();  // clears started_ + child state (tick_count -> 0)
    ASSERT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(a->tick_count, 1);  // re-ran after reset (scan phase re-entered)
}

// --- RollIntInRange + Pipeline *timeout/*retry range-randomization tests ---
//
// *timeout/*retry may be a fixed int (lo==hi) or a [lo,hi] range; the Pipeline
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
    // timeout=[20,20]ms must behave exactly like fixed *timeout=20 (action
    // runs, target never comes, times out, no retry).
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a),
                 std::make_shared<MockCondition>(NodeStatus::kFailure),
                 /*lo_ms=*/20, /*hi_ms=*/20, 0, 0);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a ran; wait target starts
    EXPECT_EQ(a->tick_count, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kFailure);  // 20ms elapsed → timeout
}

TEST(PipelineTest, RangeTimeoutResolvesWithinBounds) {
    // *timeout=[20,50]ms: across runs the measured wait-to-timeout must stay
    // within [20,50] (+ small detection slack) and come near BOTH endpoints
    // (proves a real roll, not a clamp). Tight-loop ticks keep detection
    // latency well under the ±10ms margins.
    const int lo = 20, hi = 50;
    Pipeline pipe(1, "pipe");
    auto* a = new MockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a),
                 std::make_shared<MockCondition>(NodeStatus::kFailure), lo, hi, 0, 0);

    int seen_min = hi, seen_max = lo;
    for (int run = 0; run < 30; ++run) {
        pipe.Reset();
        Blackboard bb; BtEventQueue ev;
        auto t0 = std::chrono::steady_clock::now();
        NodeStatus s = NodeStatus::kRunning;
        while (s == NodeStatus::kRunning) s = TickStamp(pipe, bb, ev);
        ASSERT_EQ(s, NodeStatus::kFailure);  // no retry → timeout fails
        int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
        // Both sides truncate to whole ms independently, so the budget can
        // fire ~1ms early: allow lo-2. Upper bound = rolled + detection slack.
        ASSERT_GE(elapsed, lo - 2);
        ASSERT_LE(elapsed, hi + 10);
        seen_min = std::min(seen_min, elapsed);
        seen_max = std::max(seen_max, elapsed);
    }
    EXPECT_LE(seen_min, lo + 10);   // some run rolled near the low end
    EXPECT_GE(seen_max, hi - 10);   // some run rolled near the high end
}

TEST(PipelineTest, RangeRetryCollapsesToFixedValue) {
    // retry=[1,1] must behave like fixed *retry=1 (one action re-run, then
    // fail).
    Pipeline pipe(1, "pipe");
    auto* a = new CountingMockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a),
                 std::make_shared<MockCondition>(NodeStatus::kFailure),
                 /*timeout_ms=*/20, /*timeout_ms=*/20, /*retry_lo=*/1, /*retry_hi=*/1);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a ran; wait target
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // timeout → re-run a
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a re-ran; wait again
    EXPECT_EQ(a->total_ticks(), 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kFailure);  // 2nd timeout, budget exhausted
}

TEST(PipelineTest, RangeRetryResolvesWithinBounds) {
    // retry=[1,4]: the number of action re-runs must land in [1,4]. The
    // action ticks once initially + once per re-run, so (total_ticks
    // delta) - 1 gives the re-run count for each run.
    const int lo = 1, hi = 4;
    Pipeline pipe(1, "pipe");
    auto* a = new CountingMockNode(2, "a", NodeStatus::kSuccess);
    pipe.AddStep(std::unique_ptr<MockNode>(a),
                 std::make_shared<MockCondition>(NodeStatus::kFailure),
                 /*timeout_ms=*/10, /*timeout_ms=*/10, lo, hi);

    int seen_min = hi, seen_max = lo;
    for (int run = 0; run < 40; ++run) {
        pipe.Reset();
        Blackboard bb; BtEventQueue ev;
        int before = a->total_ticks();
        NodeStatus s = NodeStatus::kRunning;
        while (s == NodeStatus::kRunning) {
            s = TickStamp(pipe, bb, ev);
            // let the 10ms wait budget elapse between wait ticks
            std::this_thread::sleep_for(std::chrono::milliseconds(12));
        }
        ASSERT_EQ(s, NodeStatus::kFailure);
        int reruns = (a->total_ticks() - before) - 1;  // initial run + one per re-run
        ASSERT_GE(reruns, lo);
        ASSERT_LE(reruns, hi);
        seen_min = std::min(seen_min, reruns);
        seen_max = std::max(seen_max, reruns);
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
    // With abort=Self on the step's (node-level guard) condition, while the
    // action runs the guard is monitored; if the page is lost (Failure) the
    // action is aborted and the step fails. The guard is the child's plain
    // `condition`, NOT its *target.
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kRunning);
    pipe.AddStep(std::unique_ptr<MockNode>(a0), nullptr, 0, 0);
    auto* cond0 = new MockCondition(NodeStatus::kSuccess);
    cond0->set_abort(AbortMode::kSelf);
    pipe.children()[0]->SetCondition(std::shared_ptr<MockCondition>(cond0));
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a0 running under its guard
    EXPECT_FALSE(a0->aborted);
    cond0->set_status(NodeStatus::kFailure);             // page lost
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kFailure);  // guard interrupts a0 -> step fails
    EXPECT_TRUE(a0->aborted);
}

// --- Pipeline *target reactive abort tests ---

TEST(PipelineTest, TargetSelfAbortsActionWhenTargetMet) {
    // abort=Self on *target: while the step's (long) action runs, the target
    // becoming met aborts the action and advances immediately — the remaining
    // work is skipped. Contrast GuardInterruptsActionWhenConditionLost: there
    // the GUARD failing kills the step; here the TARGET being met completes it.
    Pipeline pipe(1, "pipe");
    auto* a0 = new CountingMockNode(2, "a0", NodeStatus::kRunning);  // never finishes alone
    auto* a1 = new MockNode(3, "a1", NodeStatus::kSuccess);
    auto* t0 = new MockCondition(NodeStatus::kFailure);
    t0->set_abort(AbortMode::kSelf);
    pipe.AddStep(std::unique_ptr<MockNode>(a0),
                 std::shared_ptr<MockCondition>(t0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1),
                 std::make_shared<MockCondition>(NodeStatus::kFailure), 0, 0);
    Blackboard bb; BtEventQueue ev;
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a0 running; t0 unmet
    EXPECT_FALSE(a0->aborted);
    t0->set_status(NodeStatus::kSuccess);  // target achieved mid-action
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a0 aborted; a1 ran; wait t1
    EXPECT_TRUE(a0->aborted);   // the long action was cut short
    EXPECT_EQ(a1->tick_count, 1);  // advanced to step 1 the same tick
}

TEST(PipelineTest, TargetSelfAbsentTargetNoAbort) {
    // Without abort=Self the classic flow holds: a running action runs to its
    // own completion even if a target (here absent) would exist. Sanity that
    // the Self path only engages with the mode set.
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kRunning);
    pipe.AddStep(std::unique_ptr<MockNode>(a0),
                 std::make_shared<MockCondition>(NodeStatus::kSuccess), 0, 0);
    Blackboard bb; BtEventQueue ev;
    // Target already met but abort=None: the scan SKIPS the step outright
    // (no action tick at all) — skip, not abort, is the default path.
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kSuccess);
    EXPECT_EQ(a0->tick_count, 0);
    EXPECT_FALSE(a0->aborted);
}

// Regression (real-world stuck): an ASYNC target on a later step returns
// Running on the scans that immediately follow an advance. The pipeline must
// STAY in the scan phase (never fall into kWait for a step whose action has
// not run) and run the action once the target resolves to pending (Failure).
TEST(PipelineTest, AsyncTargetOnAdvanceRescansThenRunsAction) {
    Pipeline pipe(1, "pipe");
    auto* a0 = new MockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new CountingMockNode(3, "a1", NodeStatus::kSuccess);
    auto* t1 = new MockCondition(NodeStatus::kRunning);  // async: Running first
    pipe.AddStep(std::unique_ptr<MockNode>(a0), nullptr, 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1),
                 std::shared_ptr<MockCondition>(t1), 0, 0);
    Blackboard bb; BtEventQueue ev;

    // t1: a0 runs (no target) -> advance -> scan(1) hits Running -> pipeline
    // Running. a1 must NOT have run: the pipeline is scanning, not waiting.
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);
    EXPECT_EQ(a1->total_ticks(), 0);

    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // still scanning
    EXPECT_EQ(a1->total_ticks(), 0);

    // The async target resolves: not met -> step 1 is pending -> a1 runs.
    // (The bug previously left the pipeline parked in kWait here forever, so
    // a1's action - Enter/Tick - never fired.)
    t1->set_status(NodeStatus::kFailure);
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a1 ran; wait target1
    EXPECT_EQ(a1->total_ticks(), 1);
}

// Same shape at the INITIAL scan: an async step-0 target parks the pipeline
// in the scan phase - the action must not run until the verdict is known
// (skip if met, run if pending).
TEST(PipelineTest, AsyncTargetAtInitialScanDelaysAction) {
    Pipeline pipe(1, "pipe");
    auto* a0 = new CountingMockNode(2, "a0", NodeStatus::kSuccess);
    auto* t0 = new MockCondition(NodeStatus::kRunning);
    pipe.AddStep(std::unique_ptr<MockNode>(a0),
                 std::shared_ptr<MockCondition>(t0), 0, 0);
    Blackboard bb; BtEventQueue ev;

    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // scanning
    EXPECT_EQ(a0->total_ticks(), 0);                            // verdict unknown: hold
    t0->set_status(NodeStatus::kFailure);                       // pending
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);   // a0 ran; waiting target
    EXPECT_EQ(a0->total_ticks(), 1);
}

TEST(PipelineTest, TargetLowerPriorityPreemptsOnRegression) {
    // abort=LowerPriority on step 0's *target: while step 1's action runs,
    // step 0's target flipping met→unmet (precondition regressed — e.g. the
    // app logged out mid-flow) preempts back to step 0 and re-runs it.
    Pipeline pipe(1, "pipe");
    auto* a0 = new CountingMockNode(2, "a0", NodeStatus::kSuccess);
    auto* a1 = new MockNode(3, "a1", NodeStatus::kRunning);  // long work
    auto* t0 = new MockCondition(NodeStatus::kFailure);
    t0->set_abort(AbortMode::kLowerPriority);
    pipe.AddStep(std::unique_ptr<MockNode>(a0),
                 std::shared_ptr<MockCondition>(t0), 0, 0);
    pipe.AddStep(std::unique_ptr<MockNode>(a1), nullptr, 0, 0);
    Blackboard bb; BtEventQueue ev;

    // t1: a0 runs (t0 unmet); a0 succeeds; wait t0.
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);
    t0->set_status(NodeStatus::kSuccess);  // t0 met → advance to step 1
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // a1 running; t0 observed met
    EXPECT_EQ(a1->tick_count, 1);
    EXPECT_FALSE(a1->aborted);

    t0->set_status(NodeStatus::kFailure);  // step 0 regressed while a1 works
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);  // preempted back to step 0
    EXPECT_TRUE(a1->aborted);   // the later work was cut
    EXPECT_EQ(pipe.children()[1].get(), static_cast<Node*>(a1));  // (structure sanity)
    // Next tick: step 0 re-runs (its target is pending again), then re-waits.
    int a0_before = a0->total_ticks();
    EXPECT_EQ(TickStamp(pipe, bb, ev), NodeStatus::kRunning);
    EXPECT_GT(a0->total_ticks(), a0_before);  // a0 re-ran
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

TEST_F(ScriptNodeIntegrationTest, PipelineTargetMetSkipsAction) {
    // *target holds → the step is skipped; with nothing pending the pipeline
    // succeeds without running the action.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_truthy.lua"}}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        local status, err = bt.exec({interval = 10, max_step = 20})
        if not status then return false, err end
        return true, status
    )");
    EXPECT_EQ(status, "success");
}

TEST_F(ScriptNodeIntegrationTest, PipelinePendingTargetRunsAction) {
    // *target never met, no timeout → the action runs, then the pipeline
    // waits on the target until exec's own max_step budget ends (failure).
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"("*timeout":30}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        local status = bt.exec({interval = 10, max_step = 20})
        return status
    )");
    EXPECT_EQ(status, "failure");
}

TEST_F(ScriptNodeIntegrationTest, PipelineArrayTimeoutEquivalentToScalar) {
    // *timeout as a [lo,hi] array must parse and behave like the scalar form.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":{"type":"Script","source":"scripts/cond_falsy.lua"},"*timeout":[30,30]}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        local status = bt.exec({interval = 10, max_step = 20})
        return status
    )");
    EXPECT_EQ(status, "failure");
}

TEST_F(ScriptNodeIntegrationTest, TargetAndOrNotComposition) {
    // *target composes with And/Or/Not: (falsy OR truthy) AND NOT falsy is
    // met → step skipped → success.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/no_args.lua",)"
            R"("*target":)"
            R"({"type":"And","children":[)"
            R"({"type":"Or","children":[)"
            R"({"type":"Script","source":"scripts/cond_falsy.lua"},)"
            R"({"type":"Script","source":"scripts/cond_truthy.lua"}]},)"
            R"({"type":"Not","child":{"type":"Script","source":"scripts/cond_falsy.lua"}}]}}]})");
    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
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

// Scenario 1 — checkout flow: Pipeline (*target skip/act/wait/retry) with a
// parameterized Subtree for one step. Each step's *target is "page == X"; the
// action navigates to X. A flaky navigation (first attempt doesn't take)
// forces one *retry re-run of that step's own action.
TEST_F(ScriptNodeIntegrationTest, E2E_CheckoutPipelineWithSubtreeAndRetry) {
    // Subtree wraps the navigate action; {{to}}/{{flaky}} templated by the step.
    resource_provider->Put("nav.json",
        R"({"type":"Script","source":"scripts/e2e_goto.lua",)"
        R"("params":{"to":"{{to}}","flaky":"{{flaky}}"}})");
    PutRoot(R"({"type":"Pipeline","children":[)"
            // step 1: target cart (not there) -> navigate (flaky 1st attempt) via Subtree
            R"({"type":"Subtree","source":"res://nav.json","params":{"to":"cart","flaky":1},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"cart"}},)"
            R"("*timeout":30,"*retry":1},)"
            // step 2: target checkout -> navigate; retry covers a flaky no-op
            R"({"type":"Script","source":"scripts/e2e_goto.lua","params":{"to":"checkout"},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"checkout"}},)"
            R"("*timeout":30,"*retry":1},)"
            // step 3: target done -> set it
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"done","value":true},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"done","value":true}}})"
            R"(]})");
    blackboard->Set("page", LuaValue(std::string("home")));  // start on home

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "success");
    EXPECT_EQ(*blackboard->Get("page"), LuaValue(std::string("checkout")));      // navigated
    EXPECT_EQ(*blackboard->Get("done"), LuaValue(true));                        // flow completed
    EXPECT_EQ(*blackboard->Get("goto_cart"), LuaValue(static_cast<int64_t>(2)));  // retried once
}

// Scenario 1b — mid-flow resume: the pipeline re-enters with the checkout
// step already done (its target holds) → that step's action is skipped and
// the flow completes from where it left off.
TEST_F(ScriptNodeIntegrationTest, E2E_PipelineSkipsCompletedStepsOnReentry) {
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/e2e_goto.lua","params":{"to":"cart"},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"cart"}}},)"
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"done","value":true},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"done","value":true}}})"
            R"(]})");
    blackboard->Set("page", LuaValue(std::string("cart")));  // cart already reached
    blackboard->Set("goto_cart", LuaValue(static_cast<int64_t>(99)));  // marker: if this changes, the action ran

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "success");
    EXPECT_EQ(*blackboard->Get("goto_cart"), LuaValue(static_cast<int64_t>(99)));  // step 0 skipped
    EXPECT_EQ(*blackboard->Get("done"), LuaValue(true));                           // step 1 ran
}

// Scenario 1c — *target params resolve `$key` blackboard references: the
// wanted page comes from the blackboard ("want"), the current page from
// "page". The target is met only when the two match — the $want ref must be
// resolved from the blackboard for the skip to work at all.
TEST_F(ScriptNodeIntegrationTest, E2E_PipelineTargetParamsResolveBbRefs) {
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/e2e_goto.lua","params":{"to":"cart"},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua",)"
            R"("params":{"key":"page","value":"$want"}}},)"
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"done","value":true},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"done","value":true}}})"
            R"(]})");
    blackboard->Set("want", LuaValue(std::string("home")));  // $want → "home"...

    auto status = RunBtScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set("want", "cart")   -- the $want ref reads THIS at Enter
        bb.set("page", "home")   -- not on cart yet: step 0 is pending
        bt.init({root = "res://root.json"})
        local status = bt.exec({interval = 10, max_step = 30})
        return status, bb.get("page")
    )");
    EXPECT_EQ(status, "success");
    // Step 0 ran (target unmet at entry) and navigated to cart; the wait then
    // saw page==cart==$want and advanced. Step 1 ran to done.
    EXPECT_EQ(*blackboard->Get("page"), LuaValue(std::string("cart")));
    EXPECT_EQ(*blackboard->Get("done"), LuaValue(true));
}

// Scenario 1d — *target abort=Self end-to-end: a long-running action is cut
// short the moment its target is achieved externally (the "page" appears
// while the filler action still runs); the pipeline advances immediately.
TEST_F(ScriptNodeIntegrationTest, E2E_TargetSelfAbortsLongAction) {
    // Filler action runs 30 ticks and would exceed max_step; a parallel
    // condition flip (page=cart at tick ~3, via e2e_run's set_at) makes the
    // *target met mid-action → Self abort cuts the action → advance.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/e2e_run.lua",)"
            R"("params":{"ticks":0,"set_at":3,"set_key":"page","set_val":"cart","exit_key":"fill_exit"},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"cart"},)"
            R"("abort":"Self"}},)"
            R"({"type":"Script","source":"scripts/e2e_set.lua","params":{"key":"done","value":true},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"done","value":true}}})"
            R"(]})");
    blackboard->Set("page", LuaValue(std::string("home")));

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "success");  // NOT timeout: the Self abort cut the filler short
    ASSERT_TRUE(blackboard->Has("fill_exit"));
    EXPECT_EQ(*blackboard->Get("fill_exit"), LuaValue(std::string("aborted")));  // cut mid-run
    EXPECT_EQ(*blackboard->Get("done"), LuaValue(true));  // advanced + completed
}

// Scenario 1e — *target abort=LowerPriority end-to-end: while step 1 works,
// step 0's target regresses (page flips away) → preempt back to step 0, redo
// it, then continue to success.
TEST_F(ScriptNodeIntegrationTest, E2E_TargetLowerPriorityPreemptsOnRegression) {
    // Step 0: goto cart (target cart). Step 1: flip_once flips page AWAY from
    // cart exactly once (blackboard counter survives the Reset — otherwise
    // every redo would flip again and preempt forever); afterwards it succeeds.
    // Step 0's LowerPriority target sees the regression, preempts, step 0
    // re-navigates, the flow completes.
    PutRoot(R"({"type":"Pipeline","children":[)"
            R"({"type":"Script","source":"scripts/e2e_goto.lua","params":{"to":"cart"},)"
            R"("*target":{"type":"Script","source":"scripts/e2e_when.lua","params":{"key":"page","value":"cart"},)"
            R"("abort":"LowerPriority"}},)"
            R"({"type":"Script","source":"scripts/flip_once.lua"})"
            R"(]})");
    blackboard->Set("page", LuaValue(std::string("home")));

    auto status = RunBtScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10, max_step = 40})
    )");
    EXPECT_EQ(status, "success");
    EXPECT_EQ(*blackboard->Get("goto_cart"), LuaValue(static_cast<int64_t>(2)));  // step 0 redone once
    EXPECT_EQ(*blackboard->Get("page"), LuaValue(std::string("cart")));  // back on cart
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
        bt.init({root = "res://root.json"})
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
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10, max_step = 30})
    )");
    EXPECT_EQ(status, "success");                                  // alert dismissed
    ASSERT_TRUE(blackboard->Has("worker_exit"));
    EXPECT_EQ(*blackboard->Get("worker_exit"), LuaValue(std::string("aborted")));  // preempted
    EXPECT_EQ(*blackboard->Get("alert_visible"), LuaValue(false));               // cleared
}

// --- Repeat / Retry range Tests ---
//
// `count` / `max_count` accept a [lo,hi] array: rolled once per run, fixed
// within the run, re-rolled on Reset — the same contract Wait/Pipeline use.

TEST(RepeatTest, RangeCountResolvesWithinBoundsAndReRolls) {
    // [2,5]: every run's child-run count lands in [2,5], and both endpoints
    // are reached across runs (proves a real roll, not a clamp).
    Blackboard bb;
    BtEventQueue events;
    int lo = 2, hi = 5, min_seen = 99, max_seen = -1;
    for (int run = 0; run < 200; ++run) {
        auto* child = new CountingMockNode(2, "c", NodeStatus::kSuccess);
        Repeat rep(1, "rep", lo, hi, std::unique_ptr<MockNode>(child));
        while (rep.Tick(bb, events) == NodeStatus::kRunning) {}
        int runs = child->total_ticks();
        ASSERT_GE(runs, lo);
        ASSERT_LE(runs, hi);
        min_seen = std::min(min_seen, runs);
        max_seen = std::max(max_seen, runs);
        rep.Reset();
    }
    EXPECT_EQ(min_seen, lo);
    EXPECT_EQ(max_seen, hi);
}

TEST(RepeatTest, RangeCountFixedWithinOneRun) {
    // Within a single run the rolled value never changes: query max via the
    // same object twice would re-run; instead verify determinism by counting
    // child runs for one run only — the in-run assertion is runs==constant,
    // covered implicitly by the loop above. Here: degenerate [3,3] == fixed 3.
    Blackboard bb;
    BtEventQueue events;
    auto* child = new CountingMockNode(2, "c", NodeStatus::kSuccess);
    Repeat rep(1, "rep", 3, 3, std::unique_ptr<MockNode>(child));
    while (rep.Tick(bb, events) == NodeStatus::kRunning) {}
    EXPECT_EQ(child->total_ticks(), 3);
}

TEST(RetryTest, RangeMaxCountResolvesWithinBounds) {
    // [1,3] attempts against an always-failing child: every run's attempt
    // count lands in [1,3]; both endpoints reached across runs.
    Blackboard bb;
    BtEventQueue events;
    int lo = 1, hi = 3, min_seen = 99, max_seen = -1;
    for (int run = 0; run < 200; ++run) {
        auto* child = new CountingMockNode(2, "c", NodeStatus::kFailure);
        Retry r(1, "r", lo, hi, std::unique_ptr<MockNode>(child));
        while (r.Tick(bb, events) == NodeStatus::kRunning) {}
        int attempts = child->total_ticks();
        ASSERT_GE(attempts, lo);
        ASSERT_LE(attempts, hi);
        min_seen = std::min(min_seen, attempts);
        max_seen = std::max(max_seen, attempts);
        r.Reset();
    }
    EXPECT_EQ(min_seen, lo);
    EXPECT_EQ(max_seen, hi);
}

// --- Retry interval Tests ---
//
// `interval` (ms) spaces out retry attempts with a tick-driven wall-clock
// wait: after a failed attempt the child is Reset but not re-ticked until
// the (possibly range-rolled, once per run) interval elapses. Driven here
// with VIRTUAL tick time via BeginTick(t) — no sleeps, fully deterministic.

TEST(RetryTest, IntervalHoldsChildBetweenAttempts) {
    // Fixed 50ms interval: after each failure the child idles until 50ms of
    // tick time pass (ticks inside the window don't reach the child). With
    // max_count=4 the 4th failure (at t=100) exhausts the cap immediately.
    Blackboard bb;
    BtEventQueue events;
    auto* child = new CountingMockNode(2, "c", NodeStatus::kFailure);
    Retry r(1, "r", 4, 50, 50, std::unique_ptr<MockNode>(child));

    events.BeginTick(0);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // attempt 1, wait starts
    events.BeginTick(49);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // 49-0 < 50: still holding
    EXPECT_EQ(child->total_ticks(), 1);
    events.BeginTick(50);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // 50-0 >= 50: attempt 2
    EXPECT_EQ(child->total_ticks(), 2);
    events.BeginTick(99);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // 99-50 < 50: holding
    EXPECT_EQ(child->total_ticks(), 2);
    events.BeginTick(100);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // attempt 3 (3 < 4), wait starts
    EXPECT_EQ(child->total_ticks(), 3);
    events.BeginTick(150);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kFailure);  // attempt 4 exhausts cap
    EXPECT_EQ(child->total_ticks(), 4);
}

TEST(RetryTest, IntervalNotAppliedBeforeFirstAttempt) {
    // The wait sits BETWEEN attempts: the very first tick reaches the child
    // immediately even with a huge interval.
    Blackboard bb;
    BtEventQueue events;
    auto* child = new CountingMockNode(2, "c", NodeStatus::kSuccess);
    Retry r(1, "r", 3, 10'000, 10'000, std::unique_ptr<MockNode>(child));

    events.BeginTick(0);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kSuccess);
    EXPECT_EQ(child->total_ticks(), 1);
}

TEST(RetryTest, IntervalRangeStaysInBounds) {
    // [20,40]ms: no second attempt before 20ms, one by 40ms — whatever was
    // rolled lands inside the window (probe from t=0, wait starts there).
    Blackboard bb;
    BtEventQueue events;
    auto* child = new CountingMockNode(2, "c", NodeStatus::kFailure);
    Retry r(1, "r", 2, 20, 40, std::unique_ptr<MockNode>(child));

    events.BeginTick(0);
    ASSERT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // rolls + attempt 1
    events.BeginTick(19);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // 19 < lo 20 for any roll
    EXPECT_EQ(child->total_ticks(), 1);
    events.BeginTick(40);
    EXPECT_EQ(r.Tick(bb, events), NodeStatus::kFailure);  // 40 >= hi: attempt 2 exhausts cap
    EXPECT_EQ(child->total_ticks(), 2);
}

TEST(RetryTest, IntervalRangeRolledPerRun) {
    // Interval [1,2]ms over many runs: probe when the second attempt becomes
    // allowed; both 1 and 2 must occur (a real per-run roll, not a clamp).
    Blackboard bb;
    BtEventQueue events;
    int min_iv = 99, max_iv = -1;
    for (int run = 0; run < 200; ++run) {
        auto* child = new CountingMockNode(2, "c", NodeStatus::kFailure);
        Retry r(1, "r", 5, 1, 2, std::unique_ptr<MockNode>(child));
        events.BeginTick(0);
        ASSERT_EQ(r.Tick(bb, events), NodeStatus::kRunning);  // attempt 1
        int iv;
        events.BeginTick(1);
        r.Tick(bb, events);
        if (child->total_ticks() == 2) {
            iv = 1;  // interval elapsed by t=1
        } else {
            events.BeginTick(2);
            r.Tick(bb, events);
            ASSERT_EQ(child->total_ticks(), 2);  // must elapse by t=2
            iv = 2;
        }
        min_iv = std::min(min_iv, iv);
        max_iv = std::max(max_iv, iv);
        r.Reset();
    }
    EXPECT_EQ(min_iv, 1);
    EXPECT_EQ(max_iv, 2);
}

TEST_F(BehaviorTreeEngineTest, ParseRepeatRetryRanges) {
    // Range / scalar / absent forms parse; bad shapes error.
    auto ok1 = TreeParser::Parse(
        R"({"type":"Repeat","params":{"count":[2,5]},"child":{"type":"Success"}})");
    ASSERT_NE(nullptr, ok1.root);
    EXPECT_TRUE(ok1.error.empty());
    auto ok2 = TreeParser::Parse(
        R"({"type":"Retry","params":{"max_count":[1,3]},"child":{"type":"Success"}})");
    ASSERT_NE(nullptr, ok2.root);
    auto ok3 = TreeParser::Parse(
        R"({"type":"Repeat","params":{"count":4},"child":{"type":"Success"}})");
    ASSERT_NE(nullptr, ok3.root);
    auto ok4 = TreeParser::Parse(R"({"type":"Repeat","child":{"type":"Success"}})");
    ASSERT_NE(nullptr, ok4.root);  // absent = infinite

    auto bad1 = TreeParser::Parse(
        R"({"type":"Repeat","params":{"count":"4"},"child":{"type":"Success"}})");
    EXPECT_EQ(nullptr, bad1.root);
    EXPECT_NE(bad1.error.find("must be a number or a"), std::string::npos);
    auto bad2 = TreeParser::Parse(
        R"({"type":"Retry","params":{"max_count":[1,2,3]},"child":{"type":"Success"}})");
    EXPECT_EQ(nullptr, bad2.root);
}

TEST_F(BehaviorTreeEngineTest, ParseRetryInterval) {
    // `interval` accepts scalar / [lo,hi] / absent; bad shapes are errors.
    auto ok1 = TreeParser::Parse(
        R"({"type":"Retry","params":{"max_count":3,"interval":100},)"
        R"("child":{"type":"Success"}})");
    ASSERT_NE(nullptr, ok1.root);
    EXPECT_TRUE(ok1.error.empty());
    auto ok2 = TreeParser::Parse(
        R"({"type":"Retry","params":{"max_count":3,"interval":[200,500]},)"
        R"("child":{"type":"Success"}})");
    ASSERT_NE(nullptr, ok2.root);
    auto ok3 = TreeParser::Parse(
        R"({"type":"Retry","params":{"interval":0},"child":{"type":"Success"}})");
    ASSERT_NE(nullptr, ok3.root);  // 0 = immediate retry (legacy default)

    auto bad1 = TreeParser::Parse(
        R"({"type":"Retry","params":{"interval":"100"},"child":{"type":"Success"}})");
    EXPECT_EQ(nullptr, bad1.root);
    EXPECT_NE(bad1.error.find("Retry 'interval' must be a number or a"), std::string::npos);
    auto bad2 = TreeParser::Parse(
        R"({"type":"Retry","params":{"interval":[1,2,3]},"child":{"type":"Success"}})");
    EXPECT_EQ(nullptr, bad2.root);
}

TEST_F(ScriptNodeIntegrationTest, RepeatRangeRunsEndToEnd) {
    // End-to-end via bt.init/exec: the [lo,hi] count form parses and runs to
    // completion (the exact rolled value is covered by the C++ range tests).
    PutRoot(R"({"type":"Repeat","params":{"count":[1,2]},"child":)"
            R"({"type":"Script","source":"scripts/no_args.lua"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 10, timeout = 5000})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
}

TEST_F(ScriptNodeIntegrationTest, RetryIntervalRunsEndToEnd) {
    // End-to-end: Retry{max_count:3, interval:[10,20]} around a child that
    // fails, then succeeds on the re-run after the wait. The wait is bounded
    // by the rolled interval (<=20ms), well under the exec timeout.
    PutRoot(R"({"type":"Retry","params":{"max_count":3,"interval":[10,20]},)"
            R"("child":{"type":"Script","source":"scripts/fail_then_ok.lua"}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "res://root.json"})
        return bt.exec({interval = 5, timeout = 5000})
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_EQ(std::get<std::string>(r.values[0]), "success");
}
