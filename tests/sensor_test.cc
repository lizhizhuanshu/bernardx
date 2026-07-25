#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <filesystem>

#include "blackboard.h"
#include "blackboard_library.h"
#include "bt_library.h"
#include "file_system_code_provider.h"
#include "lua_runtime.h"

#define AWAIT_BT(lazy) async_simple::coro::syncAwait(lazy)

class SensorBtTest : public ::testing::Test {
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
    LuaRuntime::Ptr rt;
    std::string tests_dir_;
};

using SensorActivationTest = SensorBtTest;

TEST_F(SensorActivationTest, SensorOnActivePathIsActivated) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/no_args.lua',
                sensors = {'sensor_a'},
            },
            sensors = { sensor_a = {path = 'sensors/tracking_a.lua', interval = 50} },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Selector',
                children = {
                    { type = 'Script', path = 'scripts/no_args.lua', name = 'branch_a' },
                    {
                        type = 'Sequence', name = 'branch_b',
                        sensors = {'sensor_b'},
                        children = { { type = 'Script', path = 'scripts/no_args.lua' } },
                    },
                },
            },
            sensors = { sensor_b = {path = 'sensors/tracking_b.lua', interval = 50} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_FALSE(BbHas("sensor_b_entered"));
}

TEST_F(SensorActivationTest, SensorDeactivatedWhenBranchLeavesActivePath) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Selector',
                children = {
                    {
                        type = 'Sequence', name = 'branch_a',
                        sensors = {'sensor_a'},
                        children = { { type = 'Script', path = 'scripts/run_2_then_fail.lua' } },
                    },
                    { type = 'Script', path = 'scripts/no_args.lua', name = 'branch_b' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/tracking_a.lua', interval = 50} },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Selector',
                children = {
                    {
                        type = 'Sequence', name = 'branch_a',
                        sensors = {'sensor_a'},
                        children = { { type = 'Script', path = 'scripts/run_2_then_fail.lua' } },
                    },
                    {
                        type = 'Sequence', name = 'branch_b',
                        sensors = {'sensor_b'},
                        children = { { type = 'Script', path = 'scripts/run_3_ticks.lua' } },
                    },
                },
            },
            sensors = {
                sensor_a = {path = 'sensors/tracking_a.lua', interval = 50},
                sensor_b = {path = 'sensors/tracking_b.lua', interval = 50},
            },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/run_3_ticks.lua',
                sensors = {'sensor_a'},
            },
            sensors = { sensor_a = {path = 'sensors/tracking_a.lua', interval = 50} },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Selector',
                children = {
                    {
                        type = 'Sequence', name = 'branch_a',
                        sensors = {'sensor_a'},
                        children = { { type = 'Script', path = 'scripts/run_2_then_fail.lua' } },
                    },
                    {
                        type = 'Sequence', name = 'branch_b',
                        sensors = {'sensor_b'},
                        children = { { type = 'Script', path = 'scripts/run_3_ticks.lua' } },
                    },
                },
            },
            sensors = {
                sensor_a = {path = 'sensors/tracking_a.lua', interval = 50},
                sensor_b = {path = 'sensors/tracking_b.lua', interval = 50},
            },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script', path = 'scripts/no_args.lua', name = 'step_a',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'LowerPriority'},
                        },
                        sensors = {'sensor_a'},
                    },
                    { type = 'Script', path = 'scripts/run_3_ticks.lua', name = 'step_b' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(AbortSensorTest, NoAbortDeactivatesSensor) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script', path = 'scripts/no_args.lua', name = 'step_a',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'None'},
                        },
                        sensors = {'sensor_a'},
                    },
                    { type = 'Script', path = 'scripts/run_3_ticks.lua', name = 'step_b' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, BothKeepsSensorActive) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script', path = 'scripts/no_args.lua', name = 'step_a',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'Both'},
                        },
                        sensors = {'sensor_a'},
                    },
                    { type = 'Script', path = 'scripts/run_3_ticks.lua', name = 'step_b' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(AbortSensorTest, SelfAbortDeactivatesSensor) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script', path = 'scripts/no_args.lua', name = 'step_a',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'Self'},
                        },
                        sensors = {'sensor_a'},
                    },
                    { type = 'Script', path = 'scripts/run_3_ticks.lua', name = 'step_b' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, NoDecoratorDeactivatesSensor) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script', path = 'scripts/no_args.lua', name = 'step_a',
                        sensors = {'sensor_a'},
                    },
                    { type = 'Script', path = 'scripts/run_3_ticks.lua', name = 'step_b' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, SecondSensorWithAbortAlsoMonitored) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script', path = 'scripts/no_args.lua', name = 'step_a',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'LowerPriority'},
                        },
                        sensors = {'sensor_a'},
                    },
                    { type = 'Script', path = 'scripts/run_3_ticks.lua', name = 'step_b' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set('flag', true)
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Selector', name = 'L2_sel',
                        children = {
                            {
                                type = 'Sequence', name = 'L3_seq',
                                children = {
                                    {
                                        type = 'Script', path = 'scripts/no_args.lua', name = 'L4_step_a',
                                        decorators = {
                                            {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'LowerPriority'},
                                        },
                                        sensors = {'sensor_a'},
                                    },
                                    {
                                        type = 'Selector', name = 'L4_sel',
                                        children = {
                                            { type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L5_step_b', sensors = {'sensor_b'} },
                                            { type = 'Script', path = 'scripts/no_args.lua', name = 'L5_fallback' },
                                        },
                                    },
                                },
                            },
                            { type = 'Script', path = 'scripts/no_args.lua', name = 'L3_fallback' },
                        },
                    },
                    {
                        type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L2_tail',
                        sensors = {'sensor_c'},
                    },
                },
            },
            sensors = {
                sensor_a = {path = 'sensors/counting_a.lua', interval = 10},
                sensor_b = {path = 'sensors/counting_b.lua', interval = 10},
                sensor_c = {path = 'sensors/counting_c.lua', interval = 10},
            },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Sequence', name = 'L2_seq',
                        children = {
                            {
                                type = 'Script', path = 'scripts/no_args.lua', name = 'L3_instant',
                                decorators = {
                                    {type = 'BlackboardCondition', key = 'flag', operator = 'is_set', abort = 'LowerPriority'},
                                },
                                sensors = {'sensor_a'},
                            },
                            {
                                type = 'Selector', name = 'L3_sel',
                                children = {
                                    {
                                        type = 'Sequence', name = 'L4_seq',
                                        children = { { type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L5_deep' } },
                                    },
                                    { type = 'Script', path = 'scripts/no_args.lua', name = 'L4_fallback' },
                                },
                            },
                        },
                    },
                    { type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L2_tail' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(DeepSensorTest, FiveLevelNoAbortSensorDeactivatedAtDepth) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Sequence', name = 'L2_seq',
                        children = {
                            {
                                type = 'Script', path = 'scripts/no_args.lua', name = 'L3_instant',
                                sensors = {'sensor_a'},
                            },
                            {
                                type = 'Selector', name = 'L3_sel',
                                children = {
                                    {
                                        type = 'Sequence', name = 'L4_seq',
                                        children = { { type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L5_deep' } },
                                    },
                                    { type = 'Script', path = 'scripts/no_args.lua', name = 'L4_fallback' },
                                },
                            },
                        },
                    },
                    { type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L2_tail' },
                },
            },
            sensors = { sensor_a = {path = 'sensors/counting_a.lua', interval = 10} },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local bb = require('blackboard')
        bb.set('x', true)
        bb.set('y', true)
        bb.set('z', true)
        bt.ready({
            tree = {
                type = 'Sequence',
                children = {
                    {
                        type = 'Script', path = 'scripts/no_args.lua', name = 'L2_skip',
                        decorators = {
                            {type = 'BlackboardCondition', key = 'x', operator = 'is_set', abort = 'LowerPriority'},
                        },
                        sensors = {'sensor_a'},
                    },
                    {
                        type = 'Sequence', name = 'L2_inner',
                        children = {
                            {
                                type = 'Script', path = 'scripts/no_args.lua', name = 'L3_skip',
                                decorators = {
                                    {type = 'BlackboardCondition', key = 'y', operator = 'is_set', abort = 'LowerPriority'},
                                },
                                sensors = {'sensor_b'},
                            },
                            {
                                type = 'Sequence', name = 'L3_seq',
                                children = {
                                    {
                                        type = 'Script', path = 'scripts/no_args.lua', name = 'L4_skip',
                                        decorators = {
                                            {type = 'BlackboardCondition', key = 'z', operator = 'is_set', abort = 'LowerPriority'},
                                        },
                                        sensors = {'sensor_c'},
                                    },
                                    {
                                        type = 'Selector', name = 'L4_sel',
                                        children = {
                                            { type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L5_runner' },
                                            { type = 'Script', path = 'scripts/no_args.lua', name = 'L5_fb' },
                                        },
                                    },
                                },
                            },
                        },
                    },
                    { type = 'Script', path = 'scripts/run_5_ticks.lua', name = 'L2_tail' },
                },
            },
            sensors = {
                sensor_a = {path = 'sensors/counting_a.lua', interval = 10},
                sensor_b = {path = 'sensors/counting_b.lua', interval = 10},
                sensor_c = {path = 'sensors/counting_c.lua', interval = 10},
            },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/no_args.lua',
                sensors = {'hp'},
            },
            sensors = { hp = {path = 'sensors/basic.lua', interval = 50} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorWithAsyncRequire) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/no_args.lua',
                sensors = {'data'},
            },
            sensors = { data = {path = 'sensors/with_require.lua', interval = 50} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorScriptNotFound) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/no_args.lua',
                sensors = {'bad'},
            },
            sensors = { bad = {path = 'sensors/nonexistent.lua', interval = 50} },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        local status, err = bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/no_args.lua',
                sensors = {'no_tick'},
            },
            sensors = { no_tick = {path = 'sensors/missing_tick.lua', interval = 50} },
        })
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
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Sequence',
                sensors = {'hp'},
                children = { { type = 'Script', path = 'scripts/no_args.lua' } },
            },
            sensors = { hp = {path = 'sensors/basic.lua', interval = 50} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, MultipleSensorsOnTree) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/no_args.lua',
                sensors = {'hp', 'data'},
            },
            sensors = {
                hp = {path = 'sensors/basic.lua', interval = 50},
                data = {path = 'sensors/with_require.lua', interval = 100},
            },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorFullLifecycle) {
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.ready({
            tree = {
                type = 'Script',
                path = 'scripts/no_args.lua',
                sensors = {'full'},
            },
            sensors = { full = {path = 'sensors/full_lifecycle.lua', interval = 50} },
        })
        local status, err = bt.exec({interval = 10})
        if not status then return false, err end
        return true, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}
