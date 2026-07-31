#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <filesystem>

#include "blackboard.h"
#include "blackboard_library.h"
#include "bt_library.h"
#include "file_system_code_provider.h"
#include "lua_runtime.h"
#include "memory_resource_provider.h"

#define AWAIT_BT(lazy) async_simple::coro::syncAwait(lazy)

class SensorBtTest : public ::testing::Test {
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

    int64_t BbGetInt(const std::string& key) {
        auto v = blackboard->Get(key);
        if (!v.has_value()) return 0;
        auto* n = std::get_if<int64_t>(&*v);
        return n ? *n : 0;
    }

    bool BbHas(const std::string& key) {
        return blackboard->Has(key);
    }

    bool BbGetBool(const std::string& key) {
        auto v = blackboard->Get(key);
        if (!v.has_value()) return false;
        auto* b = std::get_if<bool>(&*v);
        return b && *b;
    }

    std::shared_ptr<Blackboard> blackboard;
    std::shared_ptr<BlackboardLibrary> bb_lib;
    std::shared_ptr<BehaviorTreeLibrary> lib;
    std::shared_ptr<MemoryResourceProvider> resource_provider;
    LuaRuntime::Ptr rt;
    std::string tests_dir_;
};

using SensorActivationTest = SensorBtTest;

TEST_F(SensorActivationTest, SensorOnActivePathIsActivated) {
    PutRoot(R"({"type":"Script","path":"scripts/no_args.lua","sensors":["sensor_a"]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/tracking_a.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbHas("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
}

TEST_F(SensorActivationTest, SensorOnInactiveBranchNotActivated) {
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"branch_a"},)"
            R"({"type":"Sequence","name":"branch_b","sensors":["sensor_b"],"children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua"}]}]})");
    PutSensors(R"({"sensor_b":{"path":"sensors/tracking_b.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_FALSE(BbHas("sensor_b_entered"));
}

TEST_F(SensorActivationTest, SensorDeactivatedWhenBranchLeavesActivePath) {
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Sequence","name":"branch_a","sensors":["sensor_a"],"children":[)"
            R"({"type":"Script","path":"scripts/run_2_then_fail.lua"}]},)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"branch_b"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/tracking_a.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_exited"));
}

TEST_F(SensorActivationTest, BothBranchesActivatedSequentially) {
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Sequence","name":"branch_a","sensors":["sensor_a"],"children":[)"
            R"({"type":"Script","path":"scripts/run_2_then_fail.lua"}]},)"
            R"({"type":"Sequence","name":"branch_b","sensors":["sensor_b"],"children":[)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua"}]}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/tracking_a.lua","interval":50},)"
               R"("sensor_b":{"path":"sensors/tracking_b.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_exited"));
    EXPECT_TRUE(BbGetBool("sensor_b_entered"));
    EXPECT_TRUE(BbGetBool("sensor_b_exited"));
}

TEST_F(SensorActivationTest, AllSensorsDeactivatedWhenTreeCompletes) {
    PutRoot(R"({"type":"Script","path":"scripts/run_3_ticks.lua","sensors":["sensor_a"]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/tracking_a.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_exited"));
}

TEST_F(SensorActivationTest, SensorDeactivatedWhenAnotherSelectorBranchTakesOver) {
    PutRoot(R"({"type":"Selector","children":[)"
            R"({"type":"Sequence","name":"branch_a","sensors":["sensor_a"],"children":[)"
            R"({"type":"Script","path":"scripts/run_2_then_fail.lua"}]},)"
            R"({"type":"Sequence","name":"branch_b","sensors":["sensor_b"],"children":[)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua"}]}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/tracking_a.lua","interval":50},)"
               R"("sensor_b":{"path":"sensors/tracking_b.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_exited"));
    EXPECT_TRUE(BbGetBool("sensor_b_entered"));
    EXPECT_TRUE(BbGetBool("sensor_b_exited"));
}

using AbortSensorTest = SensorBtTest;

TEST_F(AbortSensorTest, LowerPriorityKeepsSensorActive) {
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"step_a",)"
            R"("decorators":[{"type":"BlackboardCondition","key":"flag","operator":"is_set","abort":"LowerPriority"}],)"
            R"("sensors":["sensor_a"]},)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua","name":"step_b"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(AbortSensorTest, NoAbortDeactivatesSensor) {
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"step_a",)"
            R"("decorators":[{"type":"BlackboardCondition","key":"flag","operator":"is_set","abort":"None"}],)"
            R"("sensors":["sensor_a"]},)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua","name":"step_b"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, BothKeepsSensorActive) {
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"step_a",)"
            R"("decorators":[{"type":"BlackboardCondition","key":"flag","operator":"is_set","abort":"Both"}],)"
            R"("sensors":["sensor_a"]},)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua","name":"step_b"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(AbortSensorTest, SelfAbortDeactivatesSensor) {
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"step_a",)"
            R"("decorators":[{"type":"BlackboardCondition","key":"flag","operator":"is_set","abort":"Self"}],)"
            R"("sensors":["sensor_a"]},)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua","name":"step_b"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, NoDecoratorDeactivatesSensor) {
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"step_a","sensors":["sensor_a"]},)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua","name":"step_b"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, SecondSensorWithAbortAlsoMonitored) {
    PutRoot(R"({"type":"Sequence","children":[)"
            R"({"type":"Script","path":"scripts/no_args.lua","name":"step_a",)"
            R"("decorators":[{"type":"BlackboardCondition","key":"flag","operator":"is_set","abort":"LowerPriority"}],)"
            R"("sensors":["sensor_a"]},)"
            R"({"type":"Script","path":"scripts/run_3_ticks.lua","name":"step_b"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

using DeepSensorTest = SensorBtTest;

TEST_F(DeepSensorTest, FiveLevelTreeSensorActivation) {
    PutRoot(R"({"type":"Sequence","children":[{"type":"Selector","name":"L2_sel","children":[{"type":"Sequence","name":"L3_seq","children":[{"type":"Script","path":"scripts/no_args.lua","name":"L4_step_a","decorators":[{"type":"BlackboardCondition","key":"flag","operator":"is_set","abort":"LowerPriority"}],"sensors":["sensor_a"]},{"type":"Selector","name":"L4_sel","children":[{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L5_step_b","sensors":["sensor_b"]},{"type":"Script","path":"scripts/no_args.lua","name":"L5_fallback"}]}]},{"type":"Script","path":"scripts/no_args.lua","name":"L3_fallback"}]},{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L2_tail","sensors":["sensor_c"]}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10},"sensor_b":{"path":"sensors/counting_b.lua","interval":10},"sensor_c":{"path":"sensors/counting_c.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set('flag', true)
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
    EXPECT_GT(BbGetInt("sensor_b_final_count"), 0);
    EXPECT_GT(BbGetInt("sensor_c_final_count"), 0);
}

TEST_F(DeepSensorTest, FiveLevelAbortMonitoringAcrossDepths) {
    PutRoot(R"({"type":"Sequence","children":[{"type":"Sequence","name":"L2_seq","children":[{"type":"Script","path":"scripts/no_args.lua","name":"L3_instant","decorators":[{"type":"BlackboardCondition","key":"flag","operator":"is_set","abort":"LowerPriority"}],"sensors":["sensor_a"]},{"type":"Selector","name":"L3_sel","children":[{"type":"Sequence","name":"L4_seq","children":[{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L5_deep"}]},{"type":"Script","path":"scripts/no_args.lua","name":"L4_fallback"}]}]},{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L2_tail"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(DeepSensorTest, FiveLevelNoAbortSensorDeactivatedAtDepth) {
    PutRoot(R"({"type":"Sequence","children":[{"type":"Sequence","name":"L2_seq","children":[{"type":"Script","path":"scripts/no_args.lua","name":"L3_instant","sensors":["sensor_a"]},{"type":"Selector","name":"L3_sel","children":[{"type":"Sequence","name":"L4_seq","children":[{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L5_deep"}]},{"type":"Script","path":"scripts/no_args.lua","name":"L4_fallback"}]}]},{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L2_tail"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 3);
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 0);
}

TEST_F(DeepSensorTest, FiveLevelMultipleAbortSensorsAtDifferentDepths) {
    PutRoot(R"({"type":"Sequence","children":[{"type":"Script","path":"scripts/no_args.lua","name":"L2_skip","decorators":[{"type":"BlackboardCondition","key":"x","operator":"is_set","abort":"LowerPriority"}],"sensors":["sensor_a"]},{"type":"Sequence","name":"L2_inner","children":[{"type":"Script","path":"scripts/no_args.lua","name":"L3_skip","decorators":[{"type":"BlackboardCondition","key":"y","operator":"is_set","abort":"LowerPriority"}],"sensors":["sensor_b"]},{"type":"Sequence","name":"L3_seq","children":[{"type":"Script","path":"scripts/no_args.lua","name":"L4_skip","decorators":[{"type":"BlackboardCondition","key":"z","operator":"is_set","abort":"LowerPriority"}],"sensors":["sensor_c"]},{"type":"Selector","name":"L4_sel","children":[{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L5_runner"},{"type":"Script","path":"scripts/no_args.lua","name":"L5_fb"}]}]}]},{"type":"Script","path":"scripts/run_5_ticks.lua","name":"L2_tail"}]})");
    PutSensors(R"({"sensor_a":{"path":"sensors/counting_a.lua","interval":10},"sensor_b":{"path":"sensors/counting_b.lua","interval":10},"sensor_c":{"path":"sensors/counting_c.lua","interval":10}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set('x', true)
        bb.set('y', true)
        bb.set('z', true)
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
    EXPECT_GT(BbGetInt("sensor_b_final_count"), 1);
    EXPECT_GT(BbGetInt("sensor_c_final_count"), 1);
}

using SensorInitTest = SensorBtTest;

TEST_F(SensorInitTest, SensorWithBasicScript) {
    PutRoot(R"({"type":"Script","path":"scripts/no_args.lua","sensors":["hp"]})");
    PutSensors(R"({"hp":{"path":"sensors/basic.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorWithAsyncRequire) {
    PutRoot(R"({"type":"Script","path":"scripts/no_args.lua","sensors":["data"]})");
    PutSensors(R"({"data":{"path":"sensors/with_require.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorScriptNotFound) {
    PutRoot(R"({"type":"Script","path":"scripts/no_args.lua","sensors":["bad"]})");
    PutSensors(R"({"bad":{"path":"sensors/nonexistent.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({root = "@root", sensor_defs = "@sensors"})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("bad"), std::string::npos);
}

TEST_F(SensorInitTest, SensorMissingTickFunction) {
    PutRoot(R"({"type":"Script","path":"scripts/no_args.lua","sensors":["no_tick"]})");
    PutSensors(R"({"no_tick":{"path":"sensors/missing_tick.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.init({root = "@root", sensor_defs = "@sensors"})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("no_tick"), std::string::npos);
}

TEST_F(SensorInitTest, SensorOnCompositeNode) {
    PutRoot(R"({"type":"Sequence","sensors":["hp"],)"
            R"("children":[{"type":"Script","path":"scripts/no_args.lua"}]})");
    PutSensors(R"({"hp":{"path":"sensors/basic.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, MultipleSensorsOnTree) {
    PutRoot(R"({"type":"Script","path":"scripts/no_args.lua","sensors":["hp","data"]})");
    PutSensors(R"({"hp":{"path":"sensors/basic.lua","interval":50},)"
               R"("data":{"path":"sensors/with_require.lua","interval":100}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorFullLifecycle) {
    PutRoot(R"({"type":"Script","path":"scripts/no_args.lua","sensors":["full"]})");
    PutSensors(R"({"full":{"path":"sensors/full_lifecycle.lua","interval":50}})");
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.init({root = "@root", sensor_defs = "@sensors"})
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}
