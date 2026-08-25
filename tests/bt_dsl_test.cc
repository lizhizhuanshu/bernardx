// BT DSL (.bt → 引擎 JSON) 编译器测试。
//
// 正确性基线 = 与 bernard-agent2 Python compile_text 的差分 golden
// （tests/data/bt_dsl/*.bt + *.golden.json，由 tools/gen_bt_dsl_goldens.py
// 再生）；单元测试覆盖错误路径（带行号）与 registry 替换。

#include "bt_dsl.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <async_simple/coro/SyncAwait.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "behavior_tree_engine.h"
#include "blackboard.h"
#include "blackboard_library.h"
#include "bt_library.h"
#include "file_system_code_provider.h"
#include "lua_runtime.h"
#include "memory_resource_provider.h"
#include "pipeline.h"
#include "script_node.h"
#include "subtree_node.h"
#include "tree_parser.h"

#define AWAIT_DSL(lazy) async_simple::coro::syncAwait(lazy)

namespace fs = std::filesystem;

namespace {

std::string ReadFile(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

// 编译失败时返回错误串（方便断言）；成功返回空。
std::string ErrOf(const std::string& src, const std::string* reg = nullptr) {
    return bt_dsl::CompileText(src, reg).error;
}

}  // namespace

// ── Golden 差分：键序 + 内容与 Python 编译器一致 ─────────────────────────

TEST(BtDslGolden, CorpusParityWithPythonCompiler) {
    const fs::path dir = BERNARDX_TEST_DATA_DIR "/bt_dsl";
    ASSERT_TRUE(fs::exists(dir)) << dir;
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".bt") continue;
        const std::string src = ReadFile(entry.path());
        bt_dsl::DslResult res = bt_dsl::CompileText(src);
        ASSERT_TRUE(res.error.empty())
            << entry.path().filename().string() << ": " << res.error;
        fs::path golden_path = entry.path();
        golden_path.replace_extension(".golden.json");
        auto golden = nlohmann::ordered_json::parse(ReadFile(golden_path));
        // dump() 保键序（ordered_json），两侧归一化后逐字符比对
        EXPECT_EQ(res.tree.dump(), golden.dump())
            << entry.path().filename().string() << " differs from golden";
        ++checked;
    }
    EXPECT_GE(checked, 5) << "golden corpus missing";
}

// ── 词法 / 块树 ──────────────────────────────────────────────────────────

TEST(BtDslLex, RejectsTabIndent) {
    EXPECT_NE(ErrOf("t:\n\ta: wait 1ms").find("第 2 行"), std::string::npos);
    EXPECT_NE(ErrOf("t:\n\ta: wait 1ms").find("tab"), std::string::npos);
}

TEST(BtDslLex, RejectsUnknownEscape) {
    EXPECT_NE(ErrOf("t:\n  a: set_text \"**/X\" text=\"a\\qb\"").find("未知转义"),
              std::string::npos);
}

TEST(BtDslLex, RejectsUnterminatedString) {
    EXPECT_NE(ErrOf("t:\n  a: set_text \"**/X text=\"y\"").find("无法识别"),
              std::string::npos);
}

TEST(BtDslBlocks, RootLineRules) {
    EXPECT_NE(ErrOf("t: extra stuff\n  a: wait 1ms").find("根行格式"), std::string::npos);
    EXPECT_NE(ErrOf(" t:\n  a: wait 1ms").find("根行必须在行首"), std::string::npos);
    EXPECT_NE(ErrOf("").find("空输入"), std::string::npos);
}

TEST(BtDslBlocks, SiblingIndentConsistency) {
    // 同父下首个子行 indent=6，第二行 indent=5 → 同级不一致（更深缩进会静默
    // 嵌套为上一行子行，须先回到已有层级才能触发此错）
    std::string src = "t:\n  g:\n      a: wait 1ms\n     b: wait 1ms";
    EXPECT_NE(ErrOf(src).find("缩进与同级不一致"), std::string::npos);
}

TEST(BtDslBlocks, DeeperIndentSilentlyNests) {
    // 与 Python 一致：更深的缩进 = 上一行子行，不报错
    std::string src = "t:\n  a: wait 1ms\n  c: wait 1ms\n   d: wait 1ms";
    // d(3) 成为 c 的子行 → c 是 wait 行不能有子行 → 该错误
    EXPECT_NE(ErrOf(src).find("不能有子行"), std::string::npos);
}

TEST(BtDslBlocks, OnlyOneRoot) {
    EXPECT_NE(ErrOf("t:\n  a: wait 1ms\nb: wait 1ms").find("只允许一个根节点"),
              std::string::npos);
}

// ── 行解析 / 语义 ────────────────────────────────────────────────────────

TEST(BtDslParse, KeywordAsNodeNameRejected) {
    // when/otherwise 行首会先命中 choose 分支守卫，用其它关键字验证
    EXPECT_NE(ErrOf("t:\n  max: wait 1ms").find("行首应为节点名"), std::string::npos);
}

TEST(BtDslParse, UnknownVerb) {
    EXPECT_NE(ErrOf("t:\n  a: nope").find("未知动作 `nope`"), std::string::npos);
}

TEST(BtDslParse, UndefinedLetRef) {
    EXPECT_NE(ErrOf("t:\n  a: click $nope").find("未定义的常量 $nope"),
              std::string::npos);
}

TEST(BtDslParse, PositionalWithoutPrimaryRejected) {
    // tap_point 无 primary → 位置参数报错
    EXPECT_NE(ErrOf("t:\n  a: tap_point \"**/X\"").find("无主参数,不支持位置参数"),
              std::string::npos);
}

TEST(BtDslParse, PositionalDuplicateRejected) {
    EXPECT_NE(ErrOf("t:\n  a: click \"**/X\" desc=\"**/Y\"").find("重复给出"),
              std::string::npos);
}

TEST(BtDslParse, RequiresEnforced) {
    // ime_input requires value
    EXPECT_NE(ErrOf("t:\n  a: ime_input").find("需要参数 value"), std::string::npos);
    // node_count requires key
    EXPECT_NE(ErrOf("t:\n  a: until see \"**/X\" node_count").find("需要参数 key"),
              std::string::npos);
}

TEST(BtDslParse, AutoBindDescMissing) {
    EXPECT_NE(ErrOf("t:\n  a: click").find("需要 desc"), std::string::npos);
}

TEST(BtDslParse, AutoBindDescAmbiguous) {
    EXPECT_NE(ErrOf("t:\n  a: until (see \"**/A\" and see \"**/B\") click").find("多个定位条件"),
              std::string::npos);
}

TEST(BtDslParse, AutoBindDescNegatedExcluded) {
    // not see 下的定位原子不参与绑定 → 仍缺 desc
    EXPECT_NE(ErrOf("t:\n  a: until not see \"**/Ad\" click").find("需要 desc"),
              std::string::npos);
}

TEST(BtDslParse, TrailingJunkRejected) {
    EXPECT_NE(ErrOf("t:\n  a: wait 1ms extra").find("行尾有多余内容"),
              std::string::npos);
}

TEST(BtDslParse, CountNeedsOpAndNumber) {
    EXPECT_NE(ErrOf("t:\n  a: until count \"**/X\" click \"**/X\"").find("count 需要"),
              std::string::npos);
    EXPECT_NE(ErrOf("t:\n  a: until count \"**/X\" >= \"3\" click \"**/X\"")
                  .find("比较值应为数值"),
              std::string::npos);
}

TEST(BtDslParse, AttrRequiresPredicate) {
    EXPECT_NE(ErrOf("t:\n  a: until attr \"**/X\" click \"**/X\"").find("attr 需要"),
              std::string::npos);
}

TEST(BtDslParse, RepeatRejectsWhenUntilPrefix) {
    EXPECT_NE(ErrOf("t:\n  a: until see \"**/X\" repeat until see \"**/Y\" max 2:\n"
                    "    b: wait 1ms").find("repeat 不接受 until 前缀"),
              std::string::npos);
}

TEST(BtDslParse, BranchOutsideChooseRejected) {
    EXPECT_NE(ErrOf("t:\n  when see \"**/X\":\n    a: wait 1ms")
                  .find("when/otherwise 分支只能出现在 choose 下"),
              std::string::npos);
}

TEST(BtDslParse, DoubleOtherwiseRejected) {
    std::string src =
        "t:\n  c: choose:\n    otherwise:\n      a: wait 1ms\n    otherwise:\n"
        "      b: wait 1ms";
    EXPECT_NE(ErrOf(src).find("otherwise 兜底分支至多一个"), std::string::npos);
}

TEST(BtDslParse, ActionLineCannotHaveChildren) {
    EXPECT_NE(ErrOf("t:\n  a: click \"**/X\"\n    b: wait 1ms").find("不能有子行"),
              std::string::npos);
}

TEST(BtDslParse, LetRules) {
    EXPECT_NE(ErrOf("t:\n  let when = 5").find("let 名不能是关键字"), std::string::npos);
    EXPECT_NE(ErrOf("t:\n  let a = 1 2").find("let 应为"), std::string::npos);
    // let 重复定义后者覆盖
    bt_dsl::DslResult res = bt_dsl::CompileText(
        "t:\n  let a = \"x\"\n  let a = \"y\"\n  s: set_text \"**/T\" text=$a");
    ASSERT_TRUE(res.error.empty()) << res.error;
    EXPECT_EQ(res.tree["children"][0]["params"]["text"], "y");
}

// ── 编译语义 ─────────────────────────────────────────────────────────────

TEST(BtDslCompile, BbRefCompilesToInjectionContract) {
    bt_dsl::DslResult res = bt_dsl::CompileText(
        "t:\n  a: set_text \"**/T\" text=@user\n  b: set dst = @src");
    ASSERT_TRUE(res.error.empty()) << res.error;
    EXPECT_EQ(res.tree["children"][0]["params"]["text"], "$user");
    EXPECT_EQ(res.tree["children"][1]["params"]["value"], "$src");
}

TEST(BtDslCompile, SeeAttrPredicateAcceptsBbRef) {
    // see/attr 谓词值位置此前只收字符串字面量 / $ref，allow_bb=true 后 @bb_key 也通过。
    // 编译产物仍是结构化下发：`value` 走 "$key" 注入约定，see 脚本按字段拿，不做字符串拼接。
    bt_dsl::DslResult res = bt_dsl::CompileText(
        "t:\n"
        "  a: until attr \"**/X\" value == @proxy_url click \"**/X\"\n"
        "  b: until see \"**/Y\" name == @tag click \"**/Y\"\n");
    ASSERT_TRUE(res.error.empty()) << res.error;
    auto a_target = res.tree["children"][0]["*target"];
    EXPECT_EQ(a_target["type"], "Script");
    EXPECT_EQ(a_target["params"]["by"], "class_chain");
    EXPECT_EQ(a_target["params"]["desc"], "**/X");
    EXPECT_EQ(a_target["params"]["key"], "value");
    EXPECT_EQ(a_target["params"]["value"], "$proxy_url");  // @ → "$" 注入契约
    auto b_target = res.tree["children"][1]["*target"];
    EXPECT_EQ(b_target["params"]["key"], "name");
    EXPECT_EQ(b_target["params"]["value"], "$tag");
}

TEST(BtDslCompile, SeeAttrPredicateRejectsNonStringLit) {
    // 回归：加 allow_bb 后仍要把数字/布尔字面量挡在 lit 校验里，避免 see 脚本收到非字符串。
    EXPECT_NE(ErrOf("t:\n  a: until attr \"**/X\" value == 1 click \"**/X\"")
                  .find("属性值应为字符串"),
              std::string::npos);
    EXPECT_NE(ErrOf("t:\n  a: until see \"**/X\" on == true click \"**/X\"")
                  .find("属性值应为字符串"),
              std::string::npos);
}

TEST(BtDslCompile, SeeAttrPredicateRejectsInvalidBbName) {
    // 黑板键名约束与其它位置一致：@ 后必须是合法标识符（IsBbName）。
    EXPECT_NE(ErrOf("t:\n  a: until attr \"**/X\" v == @1bad click \"**/X\"")
                  .find("黑板引用 @1bad 后应为标识符"),
              std::string::npos);
}

TEST(BtDslCompile, WaitableBbGivesCleanError) {
    // Python 在此处 KeyError 崩溃（bb 无 waitable_source）——C++ 干净报错
    std::string src = "t:\n  r: repeat until bb flag exists max 2:\n    a: wait 1ms";
    EXPECT_NE(ErrOf(src).find("无可等待形态"), std::string::npos);
}

TEST(BtDslCompile, UntilBecomesEdgeTarget) {
    bt_dsl::DslResult res =
        bt_dsl::CompileText("t:\n  a: until in_app \"com.x\" open_app \"com.x\"");
    ASSERT_TRUE(res.error.empty()) << res.error;
    auto step = res.tree["children"][0];
    EXPECT_EQ(step["*target"]["type"], "Script");
    EXPECT_EQ(step["*target"]["source"], "conds/in_app.lua");
}

TEST(BtDslCompile, ContainerModsBecomePipelineDefaults) {
    bt_dsl::DslResult res = bt_dsl::CompileText(
        "t:\n  g[retry=2, timeout=[3000,5000]ms]:\n    a: wait 1ms");
    ASSERT_TRUE(res.error.empty()) << res.error;
    auto pipe = res.tree["children"][0];
    EXPECT_EQ(pipe["type"], "Pipeline");
    // 容器行 mods → Pipeline 级缺省 params.retry/timeout（不发 * 字段）
    EXPECT_EQ(pipe["params"]["retry"], 2);
    EXPECT_EQ(pipe["params"]["timeout"].dump(), "[3000,5000]");
    EXPECT_FALSE(pipe.contains("*retry"));
    EXPECT_FALSE(pipe.contains("*timeout"));
}

TEST(BtDslCompile, NonPureWhenOnContainerPutsEdgesOnWrapper) {
    bt_dsl::DslResult res = bt_dsl::CompileText(
        "t:\n  g[retry=1]: when see \"**/X\":\n    a: wait 1ms");
    ASSERT_TRUE(res.error.empty()) << res.error;
    auto outer = res.tree["children"][0];
    // 非纯 bb when → 外层 Sequence（<n>_when + <n>_body），*retry 落在外层
    EXPECT_EQ(outer["type"], "Sequence");
    EXPECT_EQ(outer["*retry"], 1);
    EXPECT_EQ(outer["children"][0]["name"], "g_when");
    EXPECT_EQ(outer["children"][1]["name"], "g_body");
    EXPECT_EQ(outer["children"][1]["type"], "Pipeline");
    // 照抄 dsl.py：容器分支先设 params 再包装 → 内层 Pipeline 仍带缺省
    EXPECT_EQ(outer["children"][1]["params"]["retry"], 1);
}

// ── Registry ─────────────────────────────────────────────────────────────

TEST(BtDslRegistry, DefaultTextParses) {
    auto j = nlohmann::ordered_json::parse(bt_dsl::DefaultRegistryText());
    EXPECT_TRUE(j.contains("actions"));
    EXPECT_TRUE(j.contains("conds"));
    EXPECT_EQ(j["subtree_dir"], "bt/subtrees");
}

TEST(BtDslRegistry, CustomRegistryReplacesDefault) {
    const std::string reg = R"json({
      "actions": { "go": { "source": "actions/go.lua" } },
      "conds": {}, "subtree_dir": "bt/sub"
    })json";
    // 默认动词不可用
    EXPECT_NE(ErrOf("t:\n  a: click \"**/X\"", &reg).find("未知动作 `click`"),
              std::string::npos);
    // 自定义动词可用；use 走自定义 subtree_dir
    bt_dsl::DslResult res =
        bt_dsl::CompileText("t:\n  a: go\n  b: use flow(x=1)", &reg);
    ASSERT_TRUE(res.error.empty()) << res.error;
    EXPECT_EQ(res.tree["children"][0]["source"], "actions/go.lua");
    EXPECT_EQ(res.tree["children"][1]["source"], "bt/sub/flow.json");
    EXPECT_EQ(res.tree["children"][1]["params"]["x"], 1);
}

TEST(BtDslRegistry, KeywordClashRejected) {
    const std::string reg =
        R"json({"actions": {"wait": {"source": "x.lua"}}})json";
    EXPECT_NE(ErrOf("t:\n  a: wait 1ms", &reg).find("动作名与关键字冲突"),
              std::string::npos);
}

TEST(BtDslRegistry, BadJsonRejected) {
    const std::string reg = "{not json";
    EXPECT_NE(ErrOf("t:\n  a: wait 1ms", &reg).find("registry"), std::string::npos);
}

// ── 路径判定 ─────────────────────────────────────────────────────────────

TEST(BtDslPath, IsBtPath) {
    EXPECT_TRUE(bt_dsl::IsBtPath("res://bt/tasks/foo.bt"));
    EXPECT_TRUE(bt_dsl::IsBtPath("foo.bt"));
    EXPECT_FALSE(bt_dsl::IsBtPath("foo.json"));
    EXPECT_FALSE(bt_dsl::IsBtPath("foo.btx"));
    EXPECT_FALSE(bt_dsl::IsBtPath("bt"));
}

// ── TreeParser 集成：.bt root / registry 选项 / Subtree .bt 兜底 ─────────

TEST(BtDslTreeParser, BtRootWithEmbeddedRegistry) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("bt/tree.bt",
                  "t1: # e2e\n"
                  "  a1: until see \"**/X\" click\n");
    auto res = AWAIT_DSL(TreeParser::LoadAndParse("res://bt/tree.bt", provider));
    ASSERT_TRUE(res.error.empty()) << res.error;
    ASSERT_NE(nullptr, res.root);
    // 根 Pipeline + 首步 Script（内嵌 registry：click → actions/click.lua；
    // until → *target 由 Pipeline 持有，见 golden 差分与 Pipeline 既有测试）
    auto* pipe = dynamic_cast<Pipeline*>(res.root.get());
    ASSERT_NE(nullptr, pipe);
    EXPECT_EQ(pipe->name(), "t1");
    ASSERT_EQ(pipe->children().size(), 1u);
    auto* script = dynamic_cast<ScriptNode*>(pipe->children()[0].get());
    ASSERT_NE(nullptr, script);
    EXPECT_EQ(script->script_path(), "actions/click.lua");
    EXPECT_EQ(script->name(), "a1");
}

TEST(BtDslTreeParser, BtDslErrorSurfacesWithPath) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("bt/bad.bt", "t:\n  a: nope\n");
    auto res = AWAIT_DSL(TreeParser::LoadAndParse("res://bt/bad.bt", provider));
    EXPECT_EQ(nullptr, res.root);
    EXPECT_NE(res.error.find("failed to compile root bt dsl"), std::string::npos);
    EXPECT_NE(res.error.find("第 2 行"), std::string::npos);
}

TEST(BtDslTreeParser, RegistryOptionReplacesEmbedded) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    provider->Put("bt/tree.bt", "t:\n  a1: run\n");
    provider->Put("bt/registry.json",
                  R"json({"actions":{"run":{"source":"scripts/counter.lua"}},"conds":{}})json");
    auto res = AWAIT_DSL(TreeParser::LoadAndParse(
        "res://bt/tree.bt", provider, nlohmann::json::object(), nullptr, nullptr,
        "res://bt/registry.json"));
    ASSERT_TRUE(res.error.empty()) << res.error;
    auto* pipe = dynamic_cast<Pipeline*>(res.root.get());
    ASSERT_NE(nullptr, pipe);
    auto* script = dynamic_cast<ScriptNode*>(pipe->children()[0].get());
    ASSERT_NE(nullptr, script);
    EXPECT_EQ(script->script_path(), "scripts/counter.lua");
}

TEST(BtDslTreeParser, SubtreeBtFallbackForJsonSource) {
    auto provider = std::make_shared<MemoryResourceProvider>();
    // `use flow` 编译产出 bt/subtrees/flow.json；此处只有 flow.bt —— 兜底加载
    provider->Put("bt/subtrees/flow.bt", "flow_inner: # sub\n  s1: wait 1ms\n");
    provider->Put("root.json",
                  R"json({"type":"Subtree","source":"res://bt/subtrees/flow.json"})json");
    auto res = AWAIT_DSL(TreeParser::LoadAndParse("res://root.json", provider));
    ASSERT_TRUE(res.error.empty()) << res.error;
    auto* sub = dynamic_cast<SubtreeNode*>(res.root.get());
    ASSERT_NE(nullptr, sub);
    auto* pipe = dynamic_cast<Pipeline*>(sub->child());
    ASSERT_NE(nullptr, pipe);
    EXPECT_EQ(pipe->name(), "flow_inner");
}

// ── bt.init Lua e2e：.bt root + registry 选项 + exec ─────────────────────

class BtDslE2eTest : public ::testing::Test {
protected:
    void SetUp() override {
        blackboard_ = std::make_shared<Blackboard>();
        bb_lib_ = std::make_shared<BlackboardLibrary>(blackboard_);
        lib_ = std::make_shared<BehaviorTreeLibrary>(blackboard_);
        tests_dir_ = std::filesystem::absolute(std::filesystem::path(__FILE__).parent_path())
                         .string();
        resource_provider_ = std::make_shared<MemoryResourceProvider>();
        rt_ = LuaRuntime::Builder()
                  .WithCodeProvider(std::make_shared<FileSystemCodeProvider>(
                      std::vector<std::string>{tests_dir_, tests_dir_ + "/scripts"}))
                  .WithResourceProvider(resource_provider_)
                  .RegisterLibrary(bb_lib_)
                  .RegisterLibrary(lib_)
                  .Create();
    }
    void TearDown() override { lib_->engine()->Stop(); }

    std::string RunLua(const std::string& code) {
        auto r = async_simple::coro::syncAwait(rt_->RunScript(code));
        EXPECT_EQ(r.status, LUA_OK);
        return std::get<std::string>(r.values[0]);
    }

    std::shared_ptr<Blackboard> blackboard_;
    std::shared_ptr<BlackboardLibrary> bb_lib_;
    std::shared_ptr<BehaviorTreeLibrary> lib_;
    std::shared_ptr<MemoryResourceProvider> resource_provider_;
    LuaRuntime::Ptr rt_;
    std::string tests_dir_;
};

TEST_F(BtDslE2eTest, BtInitRunsDotBtTree) {
    resource_provider_->Put("bt/tree.bt",
                            "e2e: # lua 端到端\n"
                            "  a1: run\n"
                            "  a2: until see \"**/X\" run\n");
    resource_provider_->Put("bt/registry.json",
        R"json({
          "actions": { "run": { "source": "scripts/bt_module.lua" } },
          "conds": { "see": { "cond_source": "scripts/cond_truthy.lua",
                              "waitable_source": "scripts/cond_truthy.lua" } }
        })json");
    auto status = RunLua(R"(
        local bt = require('bt')
        local st, err = bt.init({root = "res://bt/tree.bt",
                                 registry = "res://bt/registry.json"})
        if not st then return 'init-fail: ' .. tostring(err) end
        local st2, err2 = bt.exec({interval = 10})
        return tostring(st2) .. ':' .. tostring(err2)
    )");
    EXPECT_EQ(status, "success:");
}

TEST_F(BtDslE2eTest, BtInitDotBtDslErrorReachesLua) {
    resource_provider_->Put("bt/bad.bt", "t:\n  a: nope\n");
    auto out = RunLua(R"(
        local bt = require('bt')
        local st, err = bt.init({root = "res://bt/bad.bt"})
        return tostring(st) .. ':' .. tostring(err)
    )");
    EXPECT_NE(out.find("failed to compile root bt dsl"), std::string::npos);
    EXPECT_NE(out.find("第 2 行"), std::string::npos);
}
