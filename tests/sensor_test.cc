#include <gtest/gtest.h>

#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/SyncAwait.h>

#include <filesystem>

#include "behavior_tree_engine.h"
#include "blackboard.h"
#include "blackboard_condition.h"
#include "bt_library.h"
#include "blackboard_library.h"
#include "composite.h"
#include "decorator.h"
#include "lua_runtime.h"
#include "sensor.h"
#include "tree_parser.h"

#define AWAIT_BT(lazy) async_simple::coro::syncAwait(lazy)

TEST(TreeParserSensorTest, ParseSensorsOnComposite) {
    const char* json = R"({
        "root": {
            "type": "Sequence",
            "sensors": [
                {"name": "btn_visible", "path": "sensors/element.lua", "interval": 100},
                {"name": "page_loaded", "path": "sensors/page.lua", "interval": 200}
            ],
            "children": [
                {"type": "Script", "path": "a.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    const auto& specs = root->sensor_specs();
    ASSERT_EQ(specs.size(), 2u);
    EXPECT_EQ(specs[0].name, "btn_visible");
    EXPECT_EQ(specs[0].script_path, "sensors/element.lua");
    EXPECT_EQ(specs[0].interval_ms, 100);
    EXPECT_EQ(specs[1].name, "page_loaded");
    EXPECT_EQ(specs[1].interval_ms, 200);
}

TEST(TreeParserSensorTest, ParseSensorsOnScriptNode) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "a.lua",
            "sensors": [
                {"name": "check", "path": "sensors/check.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->sensor_specs().size(), 1u);
    EXPECT_EQ(root->sensor_specs()[0].name, "check");
    EXPECT_EQ(root->sensor_specs()[0].interval_ms, 100);
}

TEST(TreeParserSensorTest, ParseNoSensors) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "children": [
                {"type": "Script", "path": "a.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->sensor_specs().empty());
}

TEST(TreeParserSensorTest, ParseSensorMissingName) {
    const char* json = R"({
        "root": {
            "type": "Script",
            "path": "a.lua",
            "sensors": [
                {"path": "sensors/check.lua"}
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    EXPECT_TRUE(root->sensor_specs().empty());
}

TEST(TreeParserSensorTest, ParseSensorsOnNestedNode) {
    const char* json = R"({
        "root": {
            "type": "Selector",
            "children": [
                {
                    "type": "Sequence",
                    "name": "branch_a",
                    "sensors": [
                        {"name": "a_visible", "path": "sensors/a.lua"}
                    ],
                    "children": [
                        {"type": "Script", "path": "a.lua"}
                    ]
                },
                {
                    "type": "Sequence",
                    "name": "branch_b",
                    "sensors": [
                        {"name": "b_visible", "path": "sensors/b.lua"}
                    ],
                    "children": [
                        {"type": "Script", "path": "b.lua"}
                    ]
                }
            ]
        }
    })";

    auto _parse_result = TreeParser::Parse(json);
    auto root = std::move(_parse_result.root);
    ASSERT_NE(root, nullptr);
    auto* sel = dynamic_cast<Composite*>(root.get());
    ASSERT_NE(sel, nullptr);

    auto* branch_a = sel->children()[0].get();
    ASSERT_EQ(branch_a->sensor_specs().size(), 1u);
    EXPECT_EQ(branch_a->sensor_specs()[0].name, "a_visible");

    auto* branch_b = sel->children()[1].get();
    ASSERT_EQ(branch_b->sensor_specs().size(), 1u);
    EXPECT_EQ(branch_b->sensor_specs()[0].name, "b_visible");
}

TEST(SensorLifecycleTest, DeactivateAllOnLoad) {
    auto engine = std::make_shared<BehaviorTreeEngine>();

    const char* json1 = R"({
        "root": {
            "type": "Sequence",
            "sensors": [
                {"name": "s1", "path": "sensors/a.lua"}
            ],
            "children": [{"type": "Script", "path": "a.lua"}]
        }
    })";

    ASSERT_TRUE(engine->Load(json1).first);

    const char* json2 = R"({
        "root": {
            "type": "Selector",
            "children": [{"type": "Script", "path": "b.lua"}]
        }
    })";

    EXPECT_TRUE(engine->Load(json2).first);
    EXPECT_TRUE(engine->blackboard().Has("s1") == false);
}

class SensorBtTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard = std::make_shared<Blackboard>();
        bb_lib = std::make_shared<BlackboardLibrary>(blackboard);
        lib = std::make_shared<BehaviorTreeLibrary>(blackboard);
        rt = LuaRuntime::Builder()
            .RegisterLibrary(bb_lib)
            .RegisterLibrary(lib)
            .Create();

        tests_dir_ = std::filesystem::absolute(
            std::filesystem::path(__FILE__).parent_path()).string();
    }

    void TearDown() override {
        if (lib && lib->engine() && lib->engine()->IsRunning()) {
            lib->engine()->StopLoop();
            lib->engine()->Stop();
        }
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
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/no_args.lua",
                "sensors": [
                    {"name": "sensor_a", "path": "sensors/tracking_a.lua", "interval": 50}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbHas("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
}

TEST_F(SensorActivationTest, SensorOnInactiveBranchNotActivated) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Selector",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "branch_a"
                    },
                    {
                        "type": "Sequence",
                        "name": "branch_b",
                        "sensors": [
                            {"name": "sensor_b", "path": "sensors/tracking_b.lua", "interval": 50}
                        ],
                        "children": [
                            {"type": "Script", "path": "scripts/no_args.lua"}
                        ]
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_FALSE(BbHas("sensor_b_entered"));
}

TEST_F(SensorActivationTest, SensorDeactivatedWhenBranchLeavesActivePath) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Selector",
                "children": [
                    {
                        "type": "Sequence",
                        "name": "branch_a",
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/tracking_a.lua", "interval": 50}
                        ],
                        "children": [
                            {"type": "Script", "path": "scripts/run_2_then_fail.lua"}
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "branch_b"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_exited"));
}

TEST_F(SensorActivationTest, BothBranchesActivatedSequentially) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Selector",
                "children": [
                    {
                        "type": "Sequence",
                        "name": "branch_a",
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/tracking_a.lua", "interval": 50}
                        ],
                        "children": [
                            {"type": "Script", "path": "scripts/run_2_then_fail.lua"}
                        ]
                    },
                    {
                        "type": "Sequence",
                        "name": "branch_b",
                        "sensors": [
                            {"name": "sensor_b", "path": "sensors/tracking_b.lua", "interval": 50}
                        ],
                        "children": [
                            {"type": "Script", "path": "scripts/run_3_ticks.lua"}
                        ]
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_exited"));
    EXPECT_TRUE(BbGetBool("sensor_b_entered"));
    EXPECT_TRUE(BbGetBool("sensor_b_exited"));
}

TEST_F(SensorActivationTest, AllSensorsDeactivatedWhenTreeCompletes) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/run_3_ticks.lua",
                "sensors": [
                    {"name": "sensor_a", "path": "sensors/tracking_a.lua", "interval": 50}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_TRUE(BbGetBool("sensor_a_entered"));
    EXPECT_TRUE(BbGetBool("sensor_a_exited"));
}

TEST_F(SensorActivationTest, SensorDeactivatedWhenAnotherSelectorBranchTakesOver) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Selector",
                "children": [
                    {
                        "type": "Sequence",
                        "name": "branch_a",
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/tracking_a.lua", "interval": 50}
                        ],
                        "children": [
                            {"type": "Script", "path": "scripts/run_2_then_fail.lua"}
                        ]
                    },
                    {
                        "type": "Sequence",
                        "name": "branch_b",
                        "sensors": [
                            {"name": "sensor_b", "path": "sensors/tracking_b.lua", "interval": 50}
                        ],
                        "children": [
                            {"type": "Script", "path": "scripts/run_3_ticks.lua"}
                        ]
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
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
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "step_a",
                        "decorators": [
                            {"type": "BlackboardCondition", "key": "flag", "operator": "is_set", "abort": "LowerPriority"}
                        ],
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_3_ticks.lua",
                        "name": "step_b"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(AbortSensorTest, NoAbortDeactivatesSensor) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "step_a",
                        "decorators": [
                            {"type": "BlackboardCondition", "key": "flag", "operator": "is_set", "abort": "None"}
                        ],
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_3_ticks.lua",
                        "name": "step_b"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, BothKeepsSensorActive) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "step_a",
                        "decorators": [
                            {"type": "BlackboardCondition", "key": "flag", "operator": "is_set", "abort": "Both"}
                        ],
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_3_ticks.lua",
                        "name": "step_b"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(AbortSensorTest, SelfAbortDeactivatesSensor) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "step_a",
                        "decorators": [
                            {"type": "BlackboardCondition", "key": "flag", "operator": "is_set", "abort": "Self"}
                        ],
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_3_ticks.lua",
                        "name": "step_b"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, NoDecoratorDeactivatesSensor) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "step_a",
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_3_ticks.lua",
                        "name": "step_b"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 5);
}

TEST_F(AbortSensorTest, SecondSensorWithAbortAlsoMonitored) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "step_a",
                        "decorators": [
                            {"type": "BlackboardCondition", "key": "flag", "operator": "is_set", "abort": "LowerPriority"}
                        ],
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_3_ticks.lua",
                        "name": "step_b"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

using DeepSensorTest = SensorBtTest;

TEST_F(DeepSensorTest, FiveLevelTreeSensorActivation) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Selector",
                        "name": "L2_sel",
                        "children": [
                            {
                                "type": "Sequence",
                                "name": "L3_seq",
                                "children": [
                                    {
                                        "type": "Script",
                                        "path": "scripts/no_args.lua",
                                        "name": "L4_step_a",
                                        "decorators": [
                                            {"type": "BlackboardCondition", "key": "flag", "operator": "is_set", "abort": "LowerPriority"}
                                        ],
                                        "sensors": [
                                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                                        ]
                                    },
                                    {
                                        "type": "Selector",
                                        "name": "L4_sel",
                                        "children": [
                                            {
                                                "type": "Script",
                                                "path": "scripts/run_5_ticks.lua",
                                                "name": "L5_step_b",
                                                "sensors": [
                                                    {"name": "sensor_b", "path": "sensors/counting_b.lua", "interval": 10}
                                                ]
                                            },
                                            {
                                                "type": "Script",
                                                "path": "scripts/no_args.lua",
                                                "name": "L5_fallback"
                                            }
                                        ]
                                    }
                                ]
                            },
                            {
                                "type": "Script",
                                "path": "scripts/no_args.lua",
                                "name": "L3_fallback"
                            }
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_5_ticks.lua",
                        "name": "L2_tail",
                        "sensors": [
                            {"name": "sensor_c", "path": "sensors/counting_c.lua", "interval": 10}
                        ]
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
    EXPECT_GT(BbGetInt("sensor_b_final_count"), 0);
    EXPECT_GT(BbGetInt("sensor_c_final_count"), 0);
}

TEST_F(DeepSensorTest, FiveLevelAbortMonitoringAcrossDepths) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Sequence",
                        "name": "L2_seq",
                        "children": [
                            {
                                "type": "Script",
                                "path": "scripts/no_args.lua",
                                "name": "L3_instant",
                                "decorators": [
                                    {"type": "BlackboardCondition", "key": "flag", "operator": "is_set", "abort": "LowerPriority"}
                                ],
                                "sensors": [
                                    {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                                ]
                            },
                            {
                                "type": "Selector",
                                "name": "L3_sel",
                                "children": [
                                    {
                                        "type": "Sequence",
                                        "name": "L4_seq",
                                        "children": [
                                            {
                                                "type": "Script",
                                                "path": "scripts/run_5_ticks.lua",
                                                "name": "L5_deep"
                                            }
                                        ]
                                    },
                                    {
                                        "type": "Script",
                                        "path": "scripts/no_args.lua",
                                        "name": "L4_fallback"
                                    }
                                ]
                            }
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_5_ticks.lua",
                        "name": "L2_tail"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
}

TEST_F(DeepSensorTest, FiveLevelNoAbortSensorDeactivatedAtDepth) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Sequence",
                        "name": "L2_seq",
                        "children": [
                            {
                                "type": "Script",
                                "path": "scripts/no_args.lua",
                                "name": "L3_instant",
                                "sensors": [
                                    {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                                ]
                            },
                            {
                                "type": "Selector",
                                "name": "L3_sel",
                                "children": [
                                    {
                                        "type": "Sequence",
                                        "name": "L4_seq",
                                        "children": [
                                            {
                                                "type": "Script",
                                                "path": "scripts/run_5_ticks.lua",
                                                "name": "L5_deep"
                                            }
                                        ]
                                    },
                                    {
                                        "type": "Script",
                                        "path": "scripts/no_args.lua",
                                        "name": "L4_fallback"
                                    }
                                ]
                            }
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_5_ticks.lua",
                        "name": "L2_tail"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_LT(BbGetInt("sensor_a_final_count"), 3);
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 0);
}

TEST_F(DeepSensorTest, FiveLevelMultipleAbortSensorsAtDifferentDepths) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "children": [
                    {
                        "type": "Script",
                        "path": "scripts/no_args.lua",
                        "name": "L2_skip",
                        "decorators": [
                            {"type": "BlackboardCondition", "key": "x", "operator": "is_set", "abort": "LowerPriority"}
                        ],
                        "sensors": [
                            {"name": "sensor_a", "path": "sensors/counting_a.lua", "interval": 10}
                        ]
                    },
                    {
                        "type": "Sequence",
                        "name": "L2_inner",
                        "children": [
                            {
                                "type": "Script",
                                "path": "scripts/no_args.lua",
                                "name": "L3_skip",
                                "decorators": [
                                    {"type": "BlackboardCondition", "key": "y", "operator": "is_set", "abort": "LowerPriority"}
                                ],
                                "sensors": [
                                    {"name": "sensor_b", "path": "sensors/counting_b.lua", "interval": 10}
                                ]
                            },
                            {
                                "type": "Sequence",
                                "name": "L3_seq",
                                "children": [
                                    {
                                        "type": "Script",
                                        "path": "scripts/no_args.lua",
                                        "name": "L4_skip",
                                        "decorators": [
                                            {"type": "BlackboardCondition", "key": "z", "operator": "is_set", "abort": "LowerPriority"}
                                        ],
                                        "sensors": [
                                            {"name": "sensor_c", "path": "sensors/counting_c.lua", "interval": 10}
                                        ]
                                    },
                                    {
                                        "type": "Selector",
                                        "name": "L4_sel",
                                        "children": [
                                            {
                                                "type": "Script",
                                                "path": "scripts/run_5_ticks.lua",
                                                "name": "L5_runner"
                                            },
                                            {
                                                "type": "Script",
                                                "path": "scripts/no_args.lua",
                                                "name": "L5_fb"
                                            }
                                        ]
                                    }
                                ]
                            }
                        ]
                    },
                    {
                        "type": "Script",
                        "path": "scripts/run_5_ticks.lua",
                        "name": "L2_tail"
                    }
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
    EXPECT_GT(BbGetInt("sensor_a_final_count"), 1);
    EXPECT_GT(BbGetInt("sensor_b_final_count"), 1);
    EXPECT_GT(BbGetInt("sensor_c_final_count"), 1);
}

using SensorInitTest = SensorBtTest;

TEST_F(SensorInitTest, SensorWithBasicScript) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/no_args.lua",
                "sensors": [
                    {"name": "hp", "path": "sensors/basic.lua", "interval": 50}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorWithAsyncRequire) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/no_args.lua",
                "sensors": [
                    {"name": "data", "path": "sensors/with_require.lua", "interval": 50}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorScriptNotFound) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/no_args.lua",
                "sensors": [
                    {"name": "bad", "path": "sensors/nonexistent.lua", "interval": 50}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("bad"), std::string::npos);
}

TEST_F(SensorInitTest, SensorMissingTickFunction) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/no_args.lua",
                "sensors": [
                    {"name": "no_tick", "path": "sensors/missing_tick.lua", "interval": 50}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_FALSE(std::get<bool>(r.values[0]));
    auto* err = std::get_if<std::string>(&r.values[1]);
    ASSERT_NE(err, nullptr);
    EXPECT_NE(err->find("no_tick"), std::string::npos);
}

TEST_F(SensorInitTest, SensorOnCompositeNode) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Sequence",
                "sensors": [
                    {"name": "hp", "path": "sensors/basic.lua", "interval": 50}
                ],
                "children": [
                    {"type": "Script", "path": "scripts/no_args.lua"}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, MultipleSensorsOnTree) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/no_args.lua",
                "sensors": [
                    {"name": "hp", "path": "sensors/basic.lua", "interval": 50},
                    {"name": "data", "path": "sensors/with_require.lua", "interval": 100}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}

TEST_F(SensorInitTest, SensorFullLifecycle) {
    lib->SetTickIntervalMs(10);
    auto r = AWAIT_BT(rt->RunScript(R"(
        local bt = require('bt')
        bt.set_project_path(')" + tests_dir_ + R"(')
        local json = [[{
            "root": {
                "type": "Script",
                "path": "scripts/no_args.lua",
                "sensors": [
                    {"name": "full", "path": "sensors/full_lifecycle.lua", "interval": 50}
                ]
            }
        }]]
        local ok, status = bt.run(json)
        return ok, status
    )"));
    ASSERT_EQ(r.status, LUA_OK);
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_TRUE(std::get<bool>(r.values[0]));
}
