#include "bt_dsl.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// BT DSL 编译器：`.bt` 文本 → 引擎 JSON。
// 1:1 镜像 bernard-agent2 src/bernard_agent2/bt_dsl/dsl.py（函数名一一对应）；
// 键序、报错文案、边界行为均按 Python 版照抄（个别 Python 崩溃路径改为干净报错，
// 见 WaitableNode 的 bb 分支）。所有类型都在匿名命名空间内，外部只走 CompileText。

namespace bt_dsl {
namespace {

using ojson = nlohmann::ordered_json;

// ── 错误 ────────────────────────────────────────────────────────────────

struct DslError {
    int line_no = 0;  // 0 = 无行号（registry 加载等）
    std::string msg;
};

[[noreturn]] void Fail(int line_no, std::string msg) {
    throw DslError{line_no, std::move(msg)};
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// dsl.py _KEYWORDS（含 true/false）
bool IsKeyword(const std::string& s) {
    static const std::set<std::string> kKeywords = {
        "let", "when", "until", "wait", "set", "use", "repeat", "choose",
        "otherwise", "and", "or", "not", "max", "interval", "ms", "true", "false"};
    return kKeywords.count(s) > 0;
}

// @黑板引用名（dsl.py _BB_NAME_RE）：[A-Za-z_]\w*
bool IsBbName(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

// bb 键名（dsl.py re \w+）
bool IsWord(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

// ── 词法（dsl.py tokenize） ──────────────────────────────────────────────

enum class Tk { kString, kInt, kFloat, kSym, kName };

struct Token {
    Tk kind;
    std::string s;   // 字符串内容（已反转义）/ sym / name
    int64_t i = 0;
    double d = 0.0;
};

Token SymTok(std::string s) { return Token{Tk::kSym, std::move(s), 0, 0.0}; }
Token NameTok(std::string s) { return Token{Tk::kName, std::move(s), 0, 0.0}; }

bool IsNumStartDigit(const std::string& text, size_t p) {
    return p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]));
}

struct LexResult {
    std::vector<Token> tokens;
    std::string comment;  // 引号外 `#` 之后的行尾注释（.strip()）
};

// 词法规则照抄 _TOKEN_RE 的备选顺序：空白 | "字符串" | ==/!=/>=/<= |
// 单字符数符 | float | int | name（不含空白/数符/引号/!/# 的最长串）| 裸 `!`。
LexResult Tokenize(const std::string& text, int line_no) {
    LexResult out;
    std::vector<Token>& toks = out.tokens;
    size_t pos = 0;
    while (pos < text.size()) {
        char c = text[pos];
        if (c == '#') {  // 注释只在词法边界触发（字符串内的 # 已被消费）
            out.comment = Trim(text.substr(pos + 1));
            return out;
        }
        if (IsSpace(c)) {
            ++pos;
            continue;
        }
        if (c == '"') {
            size_t i = pos + 1;
            std::string s;
            bool closed = false;
            while (i < text.size()) {
                char ch = text[i];
                if (ch == '\\') {
                    if (i + 1 >= text.size()) break;  // 末尾悬空反斜杠 → 未闭合
                    char esc = text[i + 1];
                    switch (esc) {
                        case '"': s += '"'; break;
                        case '\\': s += '\\'; break;
                        case 'n': s += '\n'; break;
                        case 't': s += '\t'; break;
                        case 'r': s += '\r'; break;
                        default: Fail(line_no, "未知转义 \\" + std::string(1, esc));
                    }
                    i += 2;
                } else if (ch == '"') {
                    closed = true;
                    break;
                } else {
                    s += ch;
                    ++i;
                }
            }
            if (!closed) Fail(line_no, std::string("无法识别的字符 '") + c + "'");
            toks.push_back(Token{Tk::kString, std::move(s), 0, 0.0});
            pos = i + 1;
            continue;
        }
        // 双字符数符
        if (pos + 1 < text.size()) {
            char c2 = text[pos + 1];
            if ((c == '=' && c2 == '=') || (c == '!' && c2 == '=') ||
                (c == '>' && c2 == '=') || (c == '<' && c2 == '=')) {
                toks.push_back(SymTok(text.substr(pos, 2)));
                pos += 2;
                continue;
            }
        }
        // 单字符数符
        if (c == '>' || c == '<' || c == '=' || c == '[' || c == ']' || c == '(' ||
            c == ')' || c == ',' || c == ':') {
            toks.push_back(SymTok(std::string(1, c)));
            ++pos;
            continue;
        }
        // 数字：-?d+ (. d+)?（float 先于 int；无数字则落入 name）
        {
            size_t p = pos;
            if (text[p] == '-') ++p;
            if (IsNumStartDigit(text, p)) {
                size_t int_begin = p;
                while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) ++p;
                bool is_float = false;
                if (p + 1 < text.size() && text[p] == '.' &&
                    std::isdigit(static_cast<unsigned char>(text[p + 1]))) {
                    is_float = true;
                    ++p;
                    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) ++p;
                }
                std::string lexeme = text.substr(pos, p - pos);
                if (is_float) {
                    Token t{Tk::kFloat, std::move(lexeme), 0, 0.0};
                    t.d = std::strtod(t.s.c_str(), nullptr);
                    toks.push_back(std::move(t));
                } else {
                    Token t{Tk::kInt, std::move(lexeme), 0, 0.0};
                    t.i = std::strtoll(t.s.c_str(), nullptr, 10);
                    toks.push_back(std::move(t));
                }
                (void)int_begin;
                pos = p;
                continue;
            }
        }
        // name：最长串，止于空白/数符/引号/!/#
        {
            auto is_stop = [](char ch) {
                return IsSpace(ch) || ch == '>' || ch == '<' || ch == '=' || ch == '[' ||
                       ch == ']' || ch == '(' || ch == ')' || ch == ',' || ch == ':' ||
                       ch == '"' || ch == '!' || ch == '#';
            };
            size_t p = pos;
            while (p < text.size() && !is_stop(text[p])) ++p;
            if (p > pos) {
                toks.push_back(NameTok(text.substr(pos, p - pos)));
                pos = p;
                continue;
            }
        }
        if (c == '!') {  // 裸 `!` 是 name token（_TOKEN_RE 最后一档）
            toks.push_back(NameTok("!"));
            ++pos;
            continue;
        }
        Fail(line_no, std::string("无法识别的字符 '") + c + "'");
    }
    return out;
}

// ── AST 数据结构 ────────────────────────────────────────────────────────

// 值：("lit", 字面量) | ("ref", $常量名) | ("bb", @黑板键)
struct Value {
    std::string tag;  // "lit" | "ref" | "bb"
    ojson lit;        // tag == "lit"
    std::string name; // tag == "ref" | "bb"

    static Value Lit(ojson v) { return Value{"lit", std::move(v), ""}; }
    static Value Ref(std::string n) { return Value{"ref", ojson(), std::move(n)}; }
    static Value Bb(std::string n) { return Value{"bb", ojson(), std::move(n)}; }
};

// 有序参数表（Python dict：改写保位、新键追加）
using ParamList = std::vector<std::pair<std::string, Value>>;

const Value* FindParam(const ParamList& ps, const std::string& key) {
    for (const auto& kv : ps)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void SetParam(ParamList& ps, std::string key, Value v) {
    for (auto& kv : ps)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    ps.emplace_back(std::move(key), std::move(v));
}

// 条件表达式：and/or(kids) | not(kids[0]) | cond(name + params)
struct CExpr {
    std::string tag;  // "and" | "or" | "not" | "cond"
    std::vector<CExpr> kids;
    std::string cond_name;
    ParamList params;
};

// 修饰符/时长等标量值：int | [lo,hi]
using ScalarOrRange = ojson;

using ModList = std::vector<std::pair<std::string, ScalarOrRange>>;

const ScalarOrRange* FindMod(const ModList& ms, const std::string& key) {
    for (const auto& kv : ms)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void SetMod(ModList& ms, std::string key, ScalarOrRange v) {
    for (auto& kv : ms)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    ms.emplace_back(std::move(key), std::move(v));
}

struct AstNode {
    std::string kind;  // action/container/wait/set/use/repeat/choose（""=待定）
    std::string name;
    int line_no = 0;
    std::string description;
    ModList mods;
    std::optional<CExpr> when;
    std::optional<CExpr> until;
    // action
    std::string verb;
    ParamList args;
    std::optional<Value> positional;
    // wait
    ScalarOrRange wait_dur;
    // set
    std::string set_key;
    std::optional<Value> set_val;
    // use
    std::string use_target;
    ParamList use_args;
    // repeat
    std::optional<CExpr> rep_until;
    int rep_max = 0;
    std::optional<ScalarOrRange> rep_interval;

    std::vector<AstNode> children;  // container/repeat 体
    // choose 分支（children 不用）
    struct Branch {
        int line_no = 0;
        std::optional<CExpr> when;  // nullopt = otherwise
        std::vector<AstNode> children;
    };
    std::vector<Branch> branches;
};

struct Program {
    std::string name;
    std::string description;
    std::map<std::string, ojson> lets;  // 重复定义后者覆盖（Python dict 赋值）
    ModList mods;                       // 根行修饰符（根 Pipeline 缺省/step_* 级联域）
    std::vector<AstNode> nodes;
};

// ── 游标（替代 dsl.py 的 _at） ──────────────────────────────────────────

struct Cur {
    const std::vector<Token>* t;
    size_t pos = 0;

    const Token* At(size_t i) const { return i < t->size() ? &(*t)[i] : nullptr; }
    bool IsSym(size_t i, const char* s) const {
        const Token* tk = At(i);
        return tk && tk->kind == Tk::kSym && tk->s == s;
    }
    bool IsName(size_t i, const char* s) const {
        const Token* tk = At(i);
        return tk && tk->kind == Tk::kName && tk->s == s;
    }
    bool AtIsName(size_t i) const {
        const Token* tk = At(i);
        return tk && tk->kind == Tk::kName;
    }
    const std::string* AtNameText(size_t i) const {
        const Token* tk = At(i);
        return tk && tk->kind == Tk::kName ? &tk->s : nullptr;
    }
};

// ── 值 / 时长 / 修饰符解析（dsl.py 232-331） ────────────────────────────

Value ParseValue(Cur& c, int line_no, const std::string& what) {
    const Token* tk = c.At(c.pos);
    if (tk && tk->kind == Tk::kName && (tk->s == "true" || tk->s == "false")) {
        Value v = Value::Lit(tk->s == "true");
        ++c.pos;
        return v;
    }
    if (tk && (tk->kind == Tk::kString || tk->kind == Tk::kInt || tk->kind == Tk::kFloat)) {
        Value v = Value::Lit(tk->kind == Tk::kString    ? ojson(tk->s)
                             : tk->kind == Tk::kInt     ? ojson(tk->i)
                                                        : ojson(tk->d));
        ++c.pos;
        return v;
    }
    Fail(line_no, what + " 应为字符串、数字或布尔");
}

Value ParseRefOrValue(Cur& c, int line_no, const std::string& what, bool allow_bb = false) {
    const Token* tk = c.At(c.pos);
    if (tk && tk->kind == Tk::kName && tk->s.size() > 1 && tk->s[0] == '$') {
        Value v = Value::Ref(tk->s.substr(1));
        ++c.pos;
        return v;
    }
    if (allow_bb && tk && tk->kind == Tk::kName && tk->s.size() > 1 && tk->s[0] == '@') {
        std::string name = tk->s.substr(1);
        if (!IsBbName(name))
            Fail(line_no, what + " 黑板引用 @" + name + " 后应为标识符");
        Value v = Value::Bb(std::move(name));
        ++c.pos;
        return v;
    }
    return ParseValue(c, line_no, what);
}

Value ParseRefOrString(Cur& c, int line_no, const std::string& what, bool allow_bb = false) {
    const Token* tk = c.At(c.pos);
    if (tk && tk->kind == Tk::kName && tk->s.size() > 1 && tk->s[0] == '$') {
        Value v = Value::Ref(tk->s.substr(1));
        ++c.pos;
        return v;
    }
    if (allow_bb && tk && tk->kind == Tk::kName && tk->s.size() > 1 && tk->s[0] == '@') {
        std::string name = tk->s.substr(1);
        if (!IsBbName(name))
            Fail(line_no, what + " 黑板引用 @" + name + " 后应为标识符");
        Value v = Value::Bb(std::move(name));
        ++c.pos;
        return v;
    }
    if (tk && tk->kind == Tk::kString) {
        Value v = Value::Lit(tk->s);
        ++c.pos;
        return v;
    }
    Fail(line_no, what + " 后应为字符串");
}

int64_t ParseInt(Cur& c, int line_no, const std::string& what,
                 std::optional<int64_t> minimum = std::nullopt) {
    const Token* tk = c.At(c.pos);
    if (!tk || tk->kind != Tk::kInt) Fail(line_no, what + " 应为整数");
    if (minimum && tk->i < *minimum) Fail(line_no, what + " 应 >= " + std::to_string(*minimum));
    ++c.pos;
    return tk->i;
}

void SkipMs(Cur& c) {
    if (c.IsName(c.pos, "ms")) ++c.pos;
}

// NUM | [NUM,NUM]（lo<=hi）→ int | [lo,hi]
ScalarOrRange ParseScalarOrRange(Cur& c, int line_no, const std::string& what, int64_t minimum) {
    if (c.IsSym(c.pos, "[")) {
        ++c.pos;
        int64_t lo = ParseInt(c, line_no, what + " lo", minimum);
        if (!c.IsSym(c.pos, ",")) Fail(line_no, what + " 范围应为 [lo,hi]");
        ++c.pos;
        int64_t hi = ParseInt(c, line_no, what + " hi", minimum);
        if (!c.IsSym(c.pos, "]")) Fail(line_no, what + " 范围应为 [lo,hi]");
        ++c.pos;
        if (lo > hi) Fail(line_no, what + " 应满足 lo<=hi");
        return ojson::array({lo, hi});
    }
    return ojson(ParseInt(c, line_no, what, minimum));
}

// 时长：NUM['ms'] | [NUM,NUM]['ms'] → int | [lo,hi]（毫秒）
ScalarOrRange ParseDuration(Cur& c, int line_no, const std::string& what) {
    ScalarOrRange v = ParseScalarOrRange(c, line_no, what, 0);
    SkipMs(c);
    return v;
}

// `[retry=.., timeout=..]` / `[step_retry=.., step_timeout=..]`（顺序可交换；timeout
// 单位 ms——Python 在两个值后都跳 ms）。值 = `@key` 黑板引用（→ 引擎 "$key" 惰性
// 引用，运行期黑板给 int 或 {lo,hi} 表）| NUM | [lo,hi]。step_* = 继承域缺省（仅
// 根行/容器行——编译期级联进子树各 Pipeline 的 params）；同级 retry/step_retry
// （或 timeout/step_timeout）互斥（dsl.py _parse_modifiers）。
ModList ParseModifiers(Cur& c, int line_no) {
    ModList mods;
    if (!c.IsSym(c.pos, "[")) return mods;
    ++c.pos;
    while (true) {
        const std::string* key = c.AtNameText(c.pos);
        if (!key || (*key != "retry" && *key != "timeout" &&
                     *key != "step_retry" && *key != "step_timeout"))
            Fail(line_no, "修饰符应为 retry/timeout/step_retry/step_timeout");
        std::string k = *key;
        const bool is_step = k.rfind("step_", 0) == 0;
        const std::string base = is_step ? k.substr(5) : k;
        const std::string other = is_step ? base : "step_" + base;
        if (FindMod(mods, base) || FindMod(mods, other))
            Fail(line_no, base + " 与 " + other + " 不能同时给出");
        ++c.pos;
        if (!c.IsSym(c.pos, "=")) Fail(line_no, "修饰符 " + k + " 缺少 =");
        ++c.pos;
        ScalarOrRange v;
        const std::string* nm = c.AtNameText(c.pos);
        if (nm && nm->size() > 1 && (*nm)[0] == '@') {
            const std::string bb = nm->substr(1);
            if (!IsBbName(bb))
                Fail(line_no, "修饰符 " + k + " 黑板引用 " + *nm + " 后应为标识符");
            v = ojson("$" + bb);
            ++c.pos;
        } else {
            v = ParseScalarOrRange(c, line_no, k, 0);
        }
        SkipMs(c);
        SetMod(mods, std::move(k), std::move(v));
        if (c.IsSym(c.pos, ",")) {
            ++c.pos;
            continue;
        }
        if (c.IsSym(c.pos, "]")) {
            ++c.pos;
            return mods;
        }
        Fail(line_no, "修饰符缺少 ] 或 ,");
    }
}

// ── 缩进块树（dsl.py build_blocks） ─────────────────────────────────────

struct Block {
    int line_no = 0;
    int indent = 0;
    std::string text;
    std::string comment;
    std::string root_mods;  // 根行 `[...]` 原文（BuildBlocks 提取，ParseProgram 解析）
    std::vector<Block> children;
};

// 根行 `^(\S+?)(\[mods\])?\s*:\s*(?:#\s*(.*))?$` 的等价实现（dsl.py _ROOT_RE）：
// - 名字 = 非空白且非 `[` 的连续段（可含 `:`——head 内最后一个冒号分隔，正则
//   贪心回溯的唯一可行解）；
// - 名字后可紧跟 `[mods]` 组（内部允许空白与一层嵌套方括号——`[lo,hi]` 值），
//   组与 `:` 之间允许空白；名字与 `[` 之间不允许空白；
// - `:` 后为空白 + 可选 `# 描述` 到行尾（多余内容 → 拒）。
bool MatchRootLine(const std::string& text, std::string& name, std::string& mods,
                   std::string& desc) {
    mods.clear();
    desc.clear();
    // ① head：非空白且非 '[' 的连续段
    size_t p = 0;
    while (p < text.size() && !IsSpace(text[p]) && text[p] != '[') ++p;
    const std::string head = text.substr(0, p);
    if (head.empty()) return false;

    size_t tail_begin = std::string::npos;
    if (p < text.size() && text[p] == '[') {
        // ② 紧跟的 [mods] 组：配平扫描（可跨空白；一层嵌套括号）
        int depth = 0;
        size_t k = p;
        bool closed = false;
        for (; k < text.size(); ++k) {
            if (text[k] == '[') ++depth;
            else if (text[k] == ']') {
                if (--depth == 0) {
                    closed = true;
                    break;
                }
            }
        }
        if (!closed) return false;
        mods = text.substr(p, k - p + 1);
        name = head;
        size_t q = k + 1;
        while (q < text.size() && IsSpace(text[q])) ++q;
        if (q >= text.size() || text[q] != ':') return false;
        tail_begin = q + 1;
    } else {
        // 无紧跟 mods 组：head 内最后一个 ':' 分隔（名字可含 ':'）
        size_t colon = head.rfind(':');
        if (colon == std::string::npos) {
            // name : 形态：空白后直接 ':'
            size_t q = p;
            while (q < text.size() && IsSpace(text[q])) ++q;
            if (q >= text.size() || text[q] != ':') return false;
            name = head;
            tail_begin = q + 1;
        } else {
            name = head.substr(0, colon);
            if (name.empty()) return false;
            tail_begin = p;  // 冒号在 head 末尾，其后即 tail
        }
    }

    // ③ tail：空白 + 可选 `# 描述` 到行尾
    size_t t = tail_begin;
    while (t < text.size() && IsSpace(text[t])) ++t;
    if (t >= text.size()) return true;
    if (text[t] != '#') return false;
    ++t;
    while (t < text.size() && IsSpace(text[t])) ++t;
    desc = text.substr(t);
    return true;
}

Block BuildBlocks(const std::string& source) {
    struct Entry {
        int line_no;
        int indent;
        std::string text;
    };
    std::vector<Entry> entries;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= source.size()) {
        size_t nl = source.find('\n', pos);
        std::string raw = source.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        ++line_no;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        // 缩进前缀含 tab → 报错
        {
            std::string stripped = raw;
            size_t i = 0;
            while (i < stripped.size() && IsSpace(stripped[i])) ++i;
            if (stripped.substr(0, i).find('\t') != std::string::npos)
                Fail(line_no, "缩进不允许 tab");
        }
        std::string text = Trim(raw);
        if (!text.empty() && text[0] != '#') {
            int indent = 0;
            while (indent < static_cast<int>(raw.size()) && raw[indent] == ' ') ++indent;
            entries.push_back(Entry{line_no, indent, text});
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (entries.empty()) Fail(1, "空输入: 缺少根行 `name: #描述`");

    const Entry& first = entries[0];
    Block root{first.line_no, 0, "", "", "", {}};
    if (!MatchRootLine(first.text, root.text, root.root_mods, root.comment))
        Fail(first.line_no, "根行格式应为 `name[修饰符]: #描述`(不能带条件)");
    if (first.indent != 0) Fail(first.line_no, "根行必须在行首(无缩进)");

    std::vector<Block*> stack{&root};
    for (size_t ei = 1; ei < entries.size(); ++ei) {
        const Entry& e = entries[ei];
        auto blk = std::make_unique<Block>();
        blk->line_no = e.line_no;
        blk->indent = e.indent;
        blk->text = e.text;
        while (stack.size() > 1 && stack.back()->indent >= e.indent) stack.pop_back();
        Block* parent = stack.back();
        if (e.indent <= parent->indent)
            Fail(e.line_no, "缩进层级非法(只允许一个根节点)");
        if (!parent->children.empty() && parent->children[0].indent != e.indent)
            Fail(e.line_no, "缩进与同级不一致(期望 " +
                                std::to_string(parent->children[0].indent) + ", 实际 " +
                                std::to_string(e.indent) + ")");
        parent->children.push_back(std::move(*blk));
        stack.push_back(&parent->children.back());
    }
    return root;
}

// ── 条件表达式（dsl.py 336-427） ────────────────────────────────────────

bool IsCmpOp(const Cur& c, size_t i) {
    return c.IsSym(i, "==") || c.IsSym(i, "!=") || c.IsSym(i, ">") || c.IsSym(i, "<") ||
           c.IsSym(i, ">=") || c.IsSym(i, "<=");
}

CExpr ParseCexpr(Cur& c, int line_no, const std::vector<std::string>& end_names);
CExpr ParseCand(Cur& c, int line_no, const std::vector<std::string>& end_names);
CExpr ParseCnot(Cur& c, int line_no, const std::vector<std::string>& end_names);
CExpr ParseCatom(Cur& c, int line_no, const std::vector<std::string>& end_names);

bool InEndNames(const std::vector<std::string>& end_names, const std::string& s) {
    for (const auto& e : end_names)
        if (e == s) return true;
    return false;
}

CExpr ParseCexpr(Cur& c, int line_no, const std::vector<std::string>& end_names) {
    // or := and ('or' and)*
    CExpr node = ParseCand(c, line_no, end_names);
    CExpr out;
    out.tag = "or";
    out.kids.push_back(std::move(node));
    while (c.IsName(c.pos, "or")) {
        ++c.pos;
        out.kids.push_back(ParseCand(c, line_no, end_names));
    }
    if (out.kids.size() == 1) return std::move(out.kids[0]);
    return out;
}

CExpr ParseCand(Cur& c, int line_no, const std::vector<std::string>& end_names) {
    // and := not ('and' not)*
    CExpr node = ParseCnot(c, line_no, end_names);
    CExpr out;
    out.tag = "and";
    out.kids.push_back(std::move(node));
    while (c.IsName(c.pos, "and")) {
        ++c.pos;
        out.kids.push_back(ParseCnot(c, line_no, end_names));
    }
    if (out.kids.size() == 1) return std::move(out.kids[0]);
    return out;
}

CExpr ParseCnot(Cur& c, int line_no, const std::vector<std::string>& end_names) {
    if (c.IsName(c.pos, "not")) {
        ++c.pos;
        CExpr out;
        out.tag = "not";
        out.kids.push_back(ParseCnot(c, line_no, end_names));
        return out;
    }
    return ParseCatom(c, line_no, end_names);
}

// see 可选谓词 k == "v"（值须字符串字面量；$ref 免检）。属性名/值存在**字面**
// 键 `key`/`value` 下（结构化下发，不做谓词拼接）——照抄 dsl.py。
void ParseSeePredicate(Cur& c, int line_no, ParamList& params) {
    if (!c.AtIsName(c.pos) || !c.IsSym(c.pos + 1, "==")) return;
    const std::string key = *c.AtNameText(c.pos);
    if (IsKeyword(key)) Fail(line_no, "属性名 " + key + " 是关键字");
    c.pos += 2;
    Value v = ParseRefOrValue(c, line_no, "属性值");
    if (v.tag == "lit" && !v.lit.is_string()) Fail(line_no, "属性值应为字符串");
    SetParam(params, "key", Value::Lit(key));
    SetParam(params, "value", std::move(v));
}

CExpr ParseCatom(Cur& c, int line_no, const std::vector<std::string>& end_names) {
    if (c.IsSym(c.pos, "(")) {
        ++c.pos;
        CExpr node = ParseCexpr(c, line_no, end_names);
        if (!c.IsSym(c.pos, ")")) Fail(line_no, "条件缺少右括号");
        ++c.pos;
        return node;
    }
    const std::string* name = c.AtNameText(c.pos);
    if (!name || InEndNames(end_names, *name)) Fail(line_no, "此处应为条件");
    ++c.pos;  // 消费条件名
    CExpr out;
    out.tag = "cond";
    out.cond_name = *name;

    if (*name == "see") {
        Value desc = ParseRefOrString(c, line_no, "see");
        ParamList params;
        SetParam(params, "by", Value::Lit("class_chain"));
        SetParam(params, "desc", std::move(desc));
        ParseSeePredicate(c, line_no, params);
        out.params = std::move(params);
        return out;
    }
    if (*name == "attr") {  // ≡ 必须带 k==v 的 see（dsl.py 同样以 "see" 标记）
        Value desc = ParseRefOrString(c, line_no, "attr");
        if (!c.AtIsName(c.pos) || !c.IsSym(c.pos + 1, "=="))
            Fail(line_no, "attr 需要 `定位器 属性 == \"值\"`");
        out.cond_name = "see";
        ParamList params;
        SetParam(params, "by", Value::Lit("class_chain"));
        SetParam(params, "desc", std::move(desc));
        ParseSeePredicate(c, line_no, params);  // 已确保 name+==，谓词必被解析
        if (!FindParam(params, "value"))
            Fail(line_no, "attr 需要 `定位器 属性 == \"值\"`");  // 不可达，防御
        out.params = std::move(params);
        return out;
    }
    if (*name == "in_app") {
        Value pkg = ParseRefOrString(c, line_no, "in_app");
        SetParam(out.params, "package_name", std::move(pkg));
        return out;
    }
    if (*name == "count") {
        Value desc = ParseRefOrString(c, line_no, "count");
        if (!IsCmpOp(c, c.pos))
            Fail(line_no, "count 需要 `定位器 op 数值`(op ∈ ==/!=/>/</>=/<=)");
        std::string op = c.At(c.pos)->s;
        ++c.pos;
        const Token* vtok = c.At(c.pos);
        if (!vtok || (vtok->kind != Tk::kInt && vtok->kind != Tk::kFloat))
            Fail(line_no, "count 比较值应为数值");
        ojson num = vtok->kind == Tk::kInt ? ojson(vtok->i) : ojson(vtok->d);
        ++c.pos;
        SetParam(out.params, "by", Value::Lit("class_chain"));
        SetParam(out.params, "desc", std::move(desc));
        SetParam(out.params, "op", Value::Lit(op));
        SetParam(out.params, "value", Value::Lit(std::move(num)));
        return out;
    }
    if (*name == "bb") {
        const std::string* key = c.AtNameText(c.pos);
        if (!key || !IsWord(*key)) Fail(line_no, "bb 后应为黑板键名");
        ++c.pos;  // 消费键名（条件名已在入口消费）
        std::string op = "==";
        if (IsCmpOp(c, c.pos)) {
            op = c.At(c.pos)->s;
            ++c.pos;
        }
        SetParam(out.params, "key", Value::Lit(*key));
        if (c.IsName(c.pos, "exists")) {
            ++c.pos;
            SetParam(out.params, "op", Value::Lit("exists"));
            return out;
        }
        SetParam(out.params, "op", Value::Lit(op));
        SetParam(out.params, "value", ParseRefOrValue(c, line_no, "bb 值"));
        return out;
    }
    Fail(line_no, "未知条件 `" + *name + "`");
}

// ── 行解析（dsl.py 432-563） ─────────────────────────────────────────────

ParamList ParseKvList(Cur& c, int line_no, const std::string& what);
void RejectGuards(const AstNode& node, const std::string& what);
void ValidateNode(const AstNode& node);

// `when <cexpr>:` / `otherwise:` 分支行
AstNode::Branch ParseBranchBlock(const Block& blk) {
    LexResult lex = Tokenize(blk.text, blk.line_no);
    Cur c{&lex.tokens, 0};
    AstNode::Branch br;
    br.line_no = blk.line_no;
    if (c.IsName(0, "otherwise")) {
        if (!c.IsSym(1, ":")) Fail(blk.line_no, "otherwise 行应为 `otherwise:`");
        return br;  // 无 when = otherwise
    }
    c.pos = 1;  // 复刻 Python：分支行首 token（when）不校验、直接跳过
    br.when = ParseCexpr(c, blk.line_no, {});
    if (!c.IsSym(c.pos, ":")) Fail(blk.line_no, "分支行应为 `when <条件>:`");
    return br;
}

AstNode ParseNodeBlock(const Block& blk) {
    LexResult lex = Tokenize(blk.text, blk.line_no);
    const std::vector<Token>& tokens = lex.tokens;
    Cur c{&tokens, 0};
    const std::string* first = c.AtNameText(0);
    if (tokens.empty() || !first || IsKeyword(*first))
        Fail(blk.line_no, "行首应为节点名(非关键字)");
    AstNode node;
    node.kind = "";
    node.name = *first;
    node.line_no = blk.line_no;
    node.description = lex.comment;
    c.pos = 1;
    node.mods = ParseModifiers(c, blk.line_no);
    if (!c.IsSym(c.pos, ":")) Fail(blk.line_no, "节点名后应为 :");
    ++c.pos;

    if (c.IsName(c.pos, "when")) {
        ++c.pos;
        node.when = ParseCexpr(c, blk.line_no, {"until"});
    }
    if (c.IsName(c.pos, "until")) {
        ++c.pos;
        node.until = ParseCexpr(c, blk.line_no, {});
    }

    // 尾部 `:` = 本行带子行；仅允许出现在行尾
    if (c.IsSym(c.pos, ":") && c.pos + 1 == tokens.size()) ++c.pos;

    const std::string* verb = c.AtNameText(c.pos);
    if (verb && *verb == "wait") {
        node.kind = "wait";
        ++c.pos;
        node.wait_dur = ParseDuration(c, blk.line_no, "wait 时长");
    } else if (verb && *verb == "set") {
        const std::string* key = c.AtNameText(c.pos + 1);
        if (!key || !c.IsSym(c.pos + 2, "=")) Fail(blk.line_no, "set 应为 `set 键 = 值`");
        node.kind = "set";
        node.set_key = *key;
        c.pos += 3;
        node.set_val = ParseRefOrValue(c, blk.line_no, "set 值", /*allow_bb=*/true);
    } else if (verb && *verb == "use") {
        const std::string* target = c.AtNameText(c.pos + 1);
        if (!target) Fail(blk.line_no, "use 后应为子树名");
        node.kind = "use";
        node.use_target = *target;
        c.pos += 2;
        if (c.IsSym(c.pos, "(")) {
            ++c.pos;
            node.use_args = ParseKvList(c, blk.line_no, "use 参数");
        }
    } else if (verb && *verb == "repeat") {
        if (!c.IsName(c.pos + 1, "until"))
            Fail(blk.line_no, "repeat 应为 `repeat until <条件> max N`");
        if (node.until) Fail(blk.line_no, "repeat 不接受 until 前缀(用 repeat until)");
        if (node.when) Fail(blk.line_no, "repeat 不接受 when 前缀(用 choose 分支表达)");
        node.kind = "repeat";
        c.pos += 2;
        node.rep_until = ParseCexpr(c, blk.line_no, {"max"});
        if (!c.IsName(c.pos, "max")) Fail(blk.line_no, "repeat 需要 `max N`(防死循环)");
        ++c.pos;
        node.rep_max = static_cast<int>(ParseInt(c, blk.line_no, "repeat max", 1));
        if (c.IsName(c.pos, "interval")) {
            ++c.pos;
            node.rep_interval = ParseDuration(c, blk.line_no, "interval");
        }
    } else if (verb && *verb == "choose") {
        node.kind = "choose";
        ++c.pos;
        RejectGuards(node, "choose");
    } else if (verb && !IsKeyword(*verb)) {
        node.kind = "action";
        node.verb = *verb;
        ++c.pos;
        // 位置主参数："字面量" | $let | @黑板（按 registry primary 落参）
        const Token* pos_tok = c.At(c.pos);
        if (pos_tok && pos_tok->kind == Tk::kString) {
            node.positional = Value::Lit(pos_tok->s);
            ++c.pos;
        } else if (pos_tok && pos_tok->kind == Tk::kName && pos_tok->s.size() > 1 &&
                   pos_tok->s[0] == '$') {
            node.positional = Value::Ref(pos_tok->s.substr(1));
            ++c.pos;
        } else if (pos_tok && pos_tok->kind == Tk::kName && pos_tok->s.size() > 1 &&
                   pos_tok->s[0] == '@') {
            std::string bname = pos_tok->s.substr(1);
            if (!IsBbName(bname))
                Fail(blk.line_no, "位置参数黑板引用 @" + bname + " 后应为标识符");
            node.positional = Value::Bb(std::move(bname));
            ++c.pos;
        }
        while (c.AtIsName(c.pos) && c.IsSym(c.pos + 1, "=")) {
            std::string key = *c.AtNameText(c.pos);
            c.pos += 2;
            Value v = ParseRefOrValue(c, blk.line_no, "参数 " + key, /*allow_bb=*/true);
            SetParam(node.args, std::move(key), std::move(v));
        }
    } else if (c.pos >= tokens.size()) {
        node.kind = "container";
    } else {
        Fail(blk.line_no, "无法解析的行内容 `" + c.At(c.pos)->s + "`");
    }

    // 构造后的尾部 `:`（repeat/choose 等带子行标志）
    if (c.IsSym(c.pos, ":") && c.pos + 1 == tokens.size()) ++c.pos;
    if (c.pos < tokens.size())
        Fail(blk.line_no, "行尾有多余内容 `" + c.At(c.pos)->s + "`");
    return node;
}

// `(k=v, k2=v2)`
ParamList ParseKvList(Cur& c, int line_no, const std::string& what) {
    ParamList args;
    while (true) {
        const std::string* key = c.AtNameText(c.pos);
        if (!key) Fail(line_no, what + " 应为 `键=值` 列表");
        std::string k = *key;
        if (!c.IsSym(c.pos + 1, "=")) Fail(line_no, what + " " + k + " 缺少 =");
        c.pos += 2;
        Value v = ParseRefOrValue(c, line_no, what + " " + k, /*allow_bb=*/true);
        SetParam(args, std::move(k), std::move(v));
        if (c.IsSym(c.pos, ",")) {
            ++c.pos;
            continue;
        }
        if (c.IsSym(c.pos, ")")) {
            ++c.pos;
            return args;
        }
        Fail(line_no, what + " 缺少 , 或 )");
    }
}

void RejectGuards(const AstNode& node, const std::string& what) {
    if (node.when || node.until) Fail(node.line_no, what + " 不接受 when/until 前缀");
}

// ── 程序组装（dsl.py 568-621） ──────────────────────────────────────────

AstNode BlockToNode(const Block& blk) {
    {
        LexResult lex = Tokenize(blk.text, blk.line_no);
        Cur c{&lex.tokens, 0};
        if (c.IsName(0, "when") || c.IsName(0, "otherwise"))
            Fail(blk.line_no, "when/otherwise 分支只能出现在 choose 下");
    }
    AstNode node = ParseNodeBlock(blk);
    if (!blk.children.empty()) {
        if (node.kind == "action" || node.kind == "wait" || node.kind == "set" ||
            node.kind == "use")
            Fail(blk.line_no, node.kind + " 行不能有子行");
        if (node.kind == "choose") {
            for (const Block& child : blk.children) {
                AstNode::Branch br = ParseBranchBlock(child);
                for (const Block& sub : child.children)
                    br.children.push_back(BlockToNode(sub));
                if (br.children.empty()) Fail(child.line_no, "分支体不能为空");
                node.branches.push_back(std::move(br));
            }
        } else {
            for (const Block& child : blk.children)
                node.children.push_back(BlockToNode(child));
        }
    }
    ValidateNode(node);
    return node;
}

void ValidateNode(const AstNode& node) {
    if (node.kind.empty()) Fail(node.line_no, "when/until 前缀后缺少内容");
    if (node.kind == "container" && node.children.empty())
        Fail(node.line_no, "容器行既无内容也无子行");
    if (node.kind == "repeat" && node.children.empty())
        Fail(node.line_no, "repeat 需要循环体子行");
    if (node.kind == "choose" && node.branches.empty())
        Fail(node.line_no, "choose 需要分支子行");
    if (node.kind != "container" &&
        (FindMod(node.mods, "step_retry") || FindMod(node.mods, "step_timeout")))
        Fail(node.line_no,
             "step_retry/step_timeout 仅用于根行/容器行(叶子行用 retry/timeout)");
}

ojson TokenToLetValue(const Token& tk) {
    // Python 原样存 token 值（不做类型检查）；sym 也按字面串存
    switch (tk.kind) {
        case Tk::kString: return ojson(tk.s);
        case Tk::kInt: return ojson(tk.i);
        case Tk::kFloat: return ojson(tk.d);
        default: return ojson(tk.s);
    }
}

Program ParseProgram(const std::string& source) {
    Block root = BuildBlocks(source);
    Program prog;
    prog.name = root.text;
    prog.description = root.comment;
    if (!root.root_mods.empty()) {
        LexResult lex = Tokenize(root.root_mods, root.line_no);
        Cur c{&lex.tokens, 0};
        prog.mods = ParseModifiers(c, root.line_no);
        if (c.pos != lex.tokens.size())
            Fail(root.line_no, "根行修饰符后有多余内容");
    }

    for (const Block& blk : root.children) {
        LexResult lex = Tokenize(blk.text, blk.line_no);
        if (lex.tokens.empty() || !(lex.tokens[0].kind == Tk::kName && lex.tokens[0].s == "let")) {
            prog.nodes.push_back(BlockToNode(blk));
            continue;
        }
        const auto& t = lex.tokens;
        if (t.size() != 4 || t[1].kind != Tk::kName || !(t[2].kind == Tk::kSym && t[2].s == "="))
            Fail(blk.line_no, "let 应为 `let 名 = 值`");
        if (IsKeyword(t[1].s)) Fail(blk.line_no, "let 名不能是关键字");
        if (!blk.children.empty()) Fail(blk.line_no, "let 行不能有子行");
        prog.lets[t[1].s] = TokenToLetValue(t[3]);
    }
    return prog;
}

// ── Registry（dsl.py registry.py） ──────────────────────────────────────

struct ActionSpec {
    std::string source;
    bool has_primary = false;
    std::string primary;
    ojson fixed = ojson::object();      // 保持 registry JSON 键序
    std::vector<std::string> requires_;
    std::vector<std::string> auto_bind;
};

struct CondSpec {
    std::string cond_source;
    std::string waitable_source;  // bb 引擎原生条件，两者皆空
};

struct Registry {
    std::map<std::string, ActionSpec> actions;
    std::map<std::string, CondSpec> conds;
    std::string subtree_dir = "bt/subtrees";

    explicit Registry(const std::string& registry_text) {
        ojson j;
        try {
            j = ojson::parse(registry_text);
        } catch (const nlohmann::json::parse_error& e) {
            Fail(0, std::string("registry: JSON 解析失败: ") + e.what());
        }
        if (j.contains("actions") && j["actions"].is_object()) {
            for (auto it = j["actions"].begin(); it != j["actions"].end(); ++it) {
                ActionSpec spec;
                const ojson& v = it.value();
                if (v.contains("source") && v["source"].is_string())
                    spec.source = v["source"].get<std::string>();
                if (v.contains("primary") && v["primary"].is_string()) {
                    spec.has_primary = true;
                    spec.primary = v["primary"].get<std::string>();
                }
                if (v.contains("fixed") && v["fixed"].is_object()) spec.fixed = v["fixed"];
                if (v.contains("requires") && v["requires"].is_array())
                    for (const auto& r : v["requires"])
                        if (r.is_string()) spec.requires_.push_back(r.get<std::string>());
                if (v.contains("auto_bind") && v["auto_bind"].is_array())
                    for (const auto& r : v["auto_bind"])
                        if (r.is_string()) spec.auto_bind.push_back(r.get<std::string>());
                actions[it.key()] = std::move(spec);
            }
        }
        if (j.contains("conds") && j["conds"].is_object()) {
            for (auto it = j["conds"].begin(); it != j["conds"].end(); ++it) {
                CondSpec spec;
                const ojson& v = it.value();
                if (v.contains("cond_source") && v["cond_source"].is_string())
                    spec.cond_source = v["cond_source"].get<std::string>();
                if (v.contains("waitable_source") && v["waitable_source"].is_string())
                    spec.waitable_source = v["waitable_source"].get<std::string>();
                conds[it.key()] = std::move(spec);
            }
        }
        if (j.contains("subtree_dir") && j["subtree_dir"].is_string())
            subtree_dir = j["subtree_dir"].get<std::string>();

        // 关键字冲突校验（registry.py Registry.__init__）
        std::string clash_a, clash_c;
        for (const auto& [verb, spec] : actions)
            if (IsKeyword(verb)) clash_a += (clash_a.empty() ? "" : ", ") + verb;
        for (const auto& [name, spec] : conds)
            if (IsKeyword(name)) clash_c += (clash_c.empty() ? "" : ", ") + name;
        if (!clash_a.empty()) Fail(0, "registry.json 动作名与关键字冲突: [" + clash_a + "]");
        if (!clash_c.empty()) Fail(0, "registry.json 条件名与关键字冲突: [" + clash_c + "]");
    }

    const ActionSpec* Action(const std::string& verb) const {
        auto it = actions.find(verb);
        return it == actions.end() ? nullptr : &it->second;
    }
    const CondSpec* Cond(const std::string& name) const {
        auto it = conds.find(name);
        return it == conds.end() ? nullptr : &it->second;
    }
};

// ── 编译（dsl.py Compiler） ─────────────────────────────────────────────

// 已解析参数（键序敏感：改写保位、新键追加 —— Python dict 语义）
using JsonParams = std::vector<std::pair<std::string, ojson>>;

const ojson* FindJsonParam(const JsonParams& ps, const std::string& key) {
    for (const auto& kv : ps)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void SetJsonParam(JsonParams& ps, std::string key, ojson v) {
    for (auto& kv : ps)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    ps.emplace_back(std::move(key), std::move(v));
}

ojson JsonObjectFrom(const JsonParams& ps) {
    ojson out = ojson::object();
    for (const auto& kv : ps) out[kv.first] = kv.second;
    return out;
}

class Compiler {
public:
    Compiler(Program prog, Registry reg) : prog_(std::move(prog)), reg_(std::move(reg)) {}

    ojson Tree() {
        Scope scope = StepScope(prog_.mods, Scope{});
        ojson t;
        t["type"] = "Pipeline";
        t["name"] = prog_.name;
        t["description"] = prog_.description;
        ojson kids = ojson::array();
        for (const auto& n : prog_.nodes) kids.push_back(CompileNode(n, scope));
        t["children"] = std::move(kids);
        PipelineParams(prog_.mods, scope, t);
        return t;
    }

private:
    Program prog_;
    Registry reg_;

    // step_* 继承域（dsl.py _step_scope）：{"retry": v, "timeout": v}，本级
    // step_* 覆盖继承值（缺省继续下传）。
    using Scope = std::map<std::string, ojson>;
    static Scope StepScope(const ModList& mods, const Scope& inherited) {
        Scope out = inherited;
        if (const auto* v = FindMod(mods, "step_retry")) out["retry"] = *v;
        if (const auto* v = FindMod(mods, "step_timeout")) out["timeout"] = *v;
        return out;
    }
    // Pipeline 缺省参数（dsl.py _pipeline_params）：本级 retry/timeout 优先，
    // 否则继承域的 step_* 值；两者皆无 → 不发 params。
    static void PipelineParams(const ModList& mods, const Scope& scope, ojson& inner) {
        ojson p = ojson::object();
        bool any = false;
        for (const char* key : {"retry", "timeout"}) {
            if (const auto* own = FindMod(mods, key)) {
                p[key] = *own;
                any = true;
            } else {
                auto it = scope.find(key);
                if (it != scope.end()) {
                    p[key] = it->second;
                    any = true;
                }
            }
        }
        if (any) inner["params"] = std::move(p);
    }

    // 常量/引用解析：lit 原样 | bb → "$name"（引擎注入契约） | ref → let 查表（未定义硬 gate）
    ojson Lit(const Value& v, int line_no, const std::string& what = "值") {
        if (v.tag == "lit") return v.lit;
        if (v.tag == "bb") return "$" + v.name;
        auto it = prog_.lets.find(v.name);
        if (it == prog_.lets.end())
            Fail(line_no, "未定义的常量 $" + v.name + "(" + what + ")");
        return it->second;
    }

    static bool IsPureBb(const CExpr& c) {
        if (c.tag == "cond") return c.cond_name == "bb";
        if (c.tag == "not") return IsPureBb(c.kids[0]);
        for (const auto& k : c.kids)
            if (!IsPureBb(k)) return false;
        return true;
    }

    // 条件原子参数 → 解析后的对象（Value 走 Lit；key/op 等已是 lit）
    ojson CondParamsObject(const ParamList& params, int line_no) {
        ojson out = ojson::object();
        for (const auto& kv : params) out[kv.first] = Lit(kv.second, line_no);
        return out;
    }

    // 条件 JSON（*target / condition 位——布尔契约 conds）
    ojson CondJson(const CExpr& c, int line_no) {
        if (c.tag == "and" || c.tag == "or") {
            ojson out;
            out["type"] = c.tag == "and" ? "And" : "Or";
            ojson kids = ojson::array();
            for (const auto& k : c.kids) kids.push_back(CondJson(k, line_no));
            out["children"] = std::move(kids);
            return out;
        }
        if (c.tag == "not") {
            ojson out;
            out["type"] = "Not";
            out["child"] = CondJson(c.kids[0], line_no);
            return out;
        }
        if (c.cond_name == "see" || c.cond_name == "count") {
            const CondSpec* spec = reg_.Cond(c.cond_name);
            if (!spec) Fail(0, "未知条件 `" + c.cond_name + "`(registry.json 未登记)");
            ojson out;
            out["type"] = "Script";
            out["source"] = spec->cond_source;
            out["params"] = CondParamsObject(c.params, line_no);
            return out;
        }
        if (c.cond_name == "in_app") {
            const CondSpec* spec = reg_.Cond("in_app");
            if (!spec) Fail(0, "未知条件 `in_app`(registry.json 未登记)");
            const Value* pkg = FindParam(c.params, "package_name");
            ojson out;
            out["type"] = "Script";
            out["source"] = spec->cond_source;
            ojson p = ojson::object();
            p["package_name"] = Lit(*pkg, line_no);
            out["params"] = std::move(p);
            return out;
        }
        // bb
        ojson out;
        out["type"] = "Blackboard";
        out["params"] = CondParamsObject(c.params, line_no);
        return out;
    }

    // 可等待条件节点（repeat/choose/when 位——ScriptNode 字符串契约 conds）
    ojson WaitableNode(const CExpr& c, int line_no, const std::string& name) {
        if (c.tag == "and" || c.tag == "or") {
            ojson out;
            out["type"] = c.tag == "and" ? "Sequence" : "Selector";
            out["name"] = name;
            ojson kids = ojson::array();
            int i = 0;
            for (const auto& k : c.kids)
                kids.push_back(WaitableNode(k, line_no, name + "_" + std::to_string(i++)));
            out["children"] = std::move(kids);
            return out;
        }
        if (c.tag == "not") {
            ojson out;
            out["type"] = "Inverter";
            out["name"] = name;
            out["child"] = WaitableNode(c.kids[0], line_no, name + "_n");
            return out;
        }
        const CondSpec* spec = reg_.Cond(c.cond_name);
        if (!spec) Fail(0, "未知条件 `" + c.cond_name + "`(registry.json 未登记)");
        // Python 在 bb 上读 waitable_source 会 KeyError——这里干净报错
        if (spec->waitable_source.empty())
            Fail(line_no, "条件 " + c.cond_name + " 无可等待形态(registry 未登记 waitable_source)");
        ojson out;
        out["type"] = "Script";
        out["name"] = name;
        out["source"] = spec->waitable_source;
        out["params"] = CondParamsObject(c.params, line_no);
        return out;
    }

    // 自动绑定源收集：定位原子(see/count)与 in_app；or/not 极性下标记歧义
    struct BindInfo {
        std::vector<std::pair<const ParamList*, bool>> locators;
        std::vector<std::pair<const Value*, bool>> in_apps;
    };

    static void WalkBinds(const CExpr& c, bool negative, BindInfo& b) {
        if (c.tag == "cond") {
            if (c.cond_name == "see" || c.cond_name == "count") {
                b.locators.emplace_back(&c.params, negative);
            } else if (c.cond_name == "in_app") {
                if (const Value* v = FindParam(c.params, "package_name"))
                    b.in_apps.emplace_back(v, negative);
            }
            return;
        }
        if (c.tag == "not") {
            WalkBinds(c.kids[0], true, b);
            return;
        }
        for (const auto& sub : c.kids) WalkBinds(sub, negative || c.tag == "or", b);
    }

    // 按注册表 auto_bind 补参数：显式 > 自动绑定；歧义/缺失报错
    void AutoBind(const AstNode& node, JsonParams& params, const ActionSpec& spec) {
        BindInfo b;
        if (node.when) WalkBinds(*node.when, false, b);
        if (node.until) WalkBinds(*node.until, false, b);
        for (const auto& key : spec.auto_bind) {
            if (FindJsonParam(params, key)) continue;
            if (key == "desc") {
                std::vector<const ParamList*> usable;
                for (const auto& [p, neg] : b.locators)
                    if (!neg) usable.push_back(p);
                if (usable.empty())
                    Fail(node.line_no,
                         "动作 " + node.verb + " 需要 desc——给本行加 see/attr 条件,或显式 desc=...");
                if (usable.size() > 1)
                    Fail(node.line_no, "多个定位条件都能绑定 desc——请显式 desc=...");
                const Value* desc = FindParam(*usable[0], "desc");
                SetJsonParam(params, "desc", Lit(*desc, node.line_no));
            } else if (key == "package_name") {
                std::vector<ojson> vals;
                for (const auto& [v, neg] : b.in_apps) {
                    if (neg) continue;
                    ojson lit = Lit(*v, node.line_no);
                    bool seen = false;
                    for (const auto& e : vals)
                        if (e == lit) {
                            seen = true;
                            break;
                        }
                    if (!seen) vals.push_back(std::move(lit));
                }
                if (vals.size() > 1)
                    Fail(node.line_no, "多个 in_app 包名冲突——请显式 package_name=...");
                if (!vals.empty()) SetJsonParam(params, "package_name", std::move(vals[0]));
            }
        }
    }

    // 步级边缘参数：*target(until) + *retry/*timeout(mods)。容器行(pipeline=true)
    // 的 mods 写入 Pipeline 级缺省 params.retry/timeout（由 CompileNode 处理），不重复发 * 字段。
    void ApplyEdges(const AstNode& node, ojson& target, bool pipeline) {
        if (node.until) target["*target"] = CondJson(*node.until, node.line_no);
        if (pipeline) return;
        for (const char* key : {"retry", "timeout"})
            if (const ScalarOrRange* v = FindMod(node.mods, key))
                target[std::string("*") + key] = *v;
    }

    // when guard：纯 bb → 引擎原生 condition；其余 → 可等待条件前缀 Sequence
    ojson WrapWhen(const AstNode& node, ojson inner, bool with_description, bool pipeline = false) {
        if (!node.when) {
            ApplyEdges(node, inner, pipeline);
            if (with_description && !node.description.empty())
                inner["description"] = node.description;
            return inner;
        }
        if (IsPureBb(*node.when)) {
            inner["condition"] = CondJson(*node.when, node.line_no);
            ApplyEdges(node, inner, pipeline);
            if (with_description && !node.description.empty())
                inner["description"] = node.description;
            return inner;
        }
        ojson guard = WaitableNode(*node.when, node.line_no, node.name + "_when");
        ojson body = inner;
        body["name"] = node.name + "_body";
        body.erase("description");
        ojson out;
        out["type"] = "Sequence";
        out["name"] = node.name;
        out["children"] = ojson::array({std::move(guard), std::move(body)});
        if (with_description && !node.description.empty())
            out["description"] = node.description;
        ApplyEdges(node, out, /*pipeline=*/false);
        return out;
    }

    ojson CompileNode(const AstNode& node, const Scope& scope) {
        const int ln = node.line_no;

        if (node.kind == "container") {
            Scope inner_scope = StepScope(node.mods, scope);
            ojson inner;
            inner["type"] = "Pipeline";
            inner["name"] = node.name;
            inner["children"] = ojson::array();
            if (node.when && IsPureBb(*node.when))
                inner["condition"] = CondJson(*node.when, ln);
            if (node.until) inner["*target"] = CondJson(*node.until, ln);
            PipelineParams(node.mods, inner_scope, inner);
            ojson kids = ojson::array();
            for (const auto& ch : node.children) kids.push_back(CompileNode(ch, inner_scope));
            inner["children"] = std::move(kids);
            return WrapWhen(node, std::move(inner), /*with_description=*/true, /*pipeline=*/true);
        }

        if (node.kind == "action") {
            const ActionSpec* spec = reg_.Action(node.verb);
            if (!spec) Fail(ln, "未知动作 `" + node.verb + "`");
            ParamList args = node.args;
            if (node.positional) {
                if (!spec->has_primary)
                    Fail(ln, "动作 " + node.verb + " 无主参数,不支持位置参数——以 k=v 给参");
                if (FindParam(args, spec->primary))
                    Fail(ln, "动作 " + node.verb + " 的 " + spec->primary +
                                 " 位置参数与 k=v 重复给出");
                SetParam(args, spec->primary, *node.positional);
            }
            JsonParams params;
            for (auto it = spec->fixed.begin(); it != spec->fixed.end(); ++it)
                params.emplace_back(it.key(), it.value());
            for (const auto& kv : args)
                SetJsonParam(params, kv.first, Lit(kv.second, ln, "参数 " + kv.first));
            for (const auto& k : spec->requires_)
                if (!FindJsonParam(params, k))
                    Fail(ln, "动作 " + node.verb + " 需要参数 " + k + "=...");
            AutoBind(node, params, *spec);
            if (FindJsonParam(params, "desc") && !FindJsonParam(params, "by"))
                params.emplace_back("by", ojson("class_chain"));

            ojson inner;
            inner["type"] = "Script";
            inner["name"] = node.name;
            inner["source"] = spec->source;
            inner["params"] = JsonObjectFrom(params);
            return WrapWhen(node, std::move(inner), /*with_description=*/true);
        }

        if (node.kind == "wait") {
            ojson out;
            out["type"] = "Wait";
            out["name"] = node.name;
            ojson p = ojson::object();
            p["timeout"] = node.wait_dur;
            out["params"] = std::move(p);
            return WrapWhen(node, std::move(out), /*with_description=*/true);
        }

        if (node.kind == "set") {
            // @src → "$src" 运行期黑板拷贝（引擎 Set 契约）；$c = let 常量编译期内联
            ojson out;
            out["type"] = "Set";
            out["name"] = node.name;
            ojson p = ojson::object();
            p["key"] = node.set_key;
            p["value"] = Lit(*node.set_val, ln, "set 值");
            out["params"] = std::move(p);
            return WrapWhen(node, std::move(out), /*with_description=*/true);
        }

        if (node.kind == "use") {
            ojson out;
            out["type"] = "Subtree";
            out["name"] = node.name;
            out["source"] = reg_.subtree_dir + "/" + node.use_target + ".json";
            if (!node.use_args.empty()) {
                ojson p = ojson::object();
                for (const auto& kv : node.use_args)
                    p[kv.first] = Lit(kv.second, ln, "use 参数 " + kv.first);
                out["params"] = std::move(p);
            }
            return WrapWhen(node, std::move(out), /*with_description=*/true);
        }

        if (node.kind == "repeat") {
            const CExpr& until = *node.rep_until;
            ojson body = ojson::array();
            for (const auto& ch : node.children) body.push_back(CompileNode(ch, scope));
            body.push_back(WaitableNode(until, ln, "until_after"));

            ojson sel_kids = ojson::array();
            sel_kids.push_back(WaitableNode(until, ln, "until"));
            ojson seq;
            seq["type"] = "Sequence";
            seq["name"] = "body";
            seq["children"] = std::move(body);
            sel_kids.push_back(std::move(seq));
            ojson sel;
            sel["type"] = "Selector";
            sel["name"] = node.name + "_sel";
            sel["children"] = std::move(sel_kids);

            ojson out;
            out["type"] = "Retry";
            out["name"] = node.name;
            ojson p = ojson::object();
            p["max_count"] = node.rep_max;
            if (node.rep_interval) p["interval"] = *node.rep_interval;
            out["params"] = std::move(p);
            out["*target"] = CondJson(until, ln);
            out["child"] = std::move(sel);
            return WrapWhen(node, std::move(out), /*with_description=*/true);
        }

        if (node.kind == "choose") {
            ojson branches = ojson::array();
            int n_otherwise = 0;
            int i = 0;
            for (const auto& br : node.branches) {
                ++i;
                ojson seq;
                seq["type"] = "Sequence";
                seq["name"] = "branch_" + std::to_string(i);
                seq["children"] = ojson::array();
                if (!br.when) {
                    if (++n_otherwise > 1) Fail(br.line_no, "otherwise 兜底分支至多一个");
                } else {
                    seq["children"].push_back(WaitableNode(*br.when, br.line_no, "when"));
                }
                for (const auto& ch : br.children)
                    seq["children"].push_back(CompileNode(ch, scope));
                branches.push_back(std::move(seq));
            }
            ojson out;
            out["type"] = "Selector";
            out["name"] = node.name;
            out["children"] = std::move(branches);
            return WrapWhen(node, std::move(out), /*with_description=*/true);
        }

        Fail(ln, "未知节点类型 " + node.kind);
    }
};

}  // namespace

DslResult CompileText(const std::string& source, const std::string* registry_text) {
    DslResult res;
    try {
        Registry reg(registry_text ? *registry_text : DefaultRegistryText());
        Program prog = ParseProgram(source);
        Compiler compiler(std::move(prog), std::move(reg));
        res.tree = compiler.Tree();
        return res;
    } catch (const DslError& e) {
        res.tree = nlohmann::ordered_json();
        res.error =
            e.line_no > 0 ? "第 " + std::to_string(e.line_no) + " 行: " + e.msg : e.msg;
        return res;
    }
}

bool IsBtPath(const std::string& path) {
    return path.size() >= 3 && path.compare(path.size() - 3, 3, ".bt") == 0;
}

// bernard-agent2 src/bernard_agent2/bt_dsl/registry.json 原文拷贝（同步于
// 2026-08；两侧契约变更时须同步此处）。
const char* DefaultRegistryText() {
    return R"json({
  "actions": {
    "click":         { "source": "actions/click.lua", "primary": "desc", "auto_bind": ["desc"] },
    "swipe_on_node": { "source": "actions/swipe_on_node.lua", "primary": "desc", "auto_bind": ["desc"] },
    "node_count":    { "source": "actions/node_count.lua", "primary": "desc", "auto_bind": ["desc"], "requires": ["key"] },
    "set_text":      { "source": "actions/set_text.lua", "primary": "desc", "auto_bind": ["desc"] },
    "ime_input":     { "source": "actions/ime_action.lua", "fixed": { "action": "input" }, "requires": ["value"] },
    "ime_delete":    { "source": "actions/ime_action.lua", "fixed": { "action": "delete" } },
    "ime_enter":     { "source": "actions/ime_action.lua", "fixed": { "action": "input", "value": "\n" } },
    "open_app":      { "source": "actions/open_app.lua", "primary": "package_name", "auto_bind": ["package_name"] },
    "close_app":     { "source": "actions/close_app.lua", "primary": "package_name", "auto_bind": ["package_name"] },
    "tap_point":     { "source": "actions/tap_xy.lua" },
    "press_key":     { "source": "actions/press_key.lua" },
    "escalate":      { "source": "actions/escalate.lua",
                       "doc": "失败升级——宣告此路不通，回退 agent 重新探索（bt_runner 解析 __BT_ESCALATE__ 标记）",
                       "params_doc": "{note=升级原因}" }
  },
  "conds": {
    "see":    { "cond_source": "conds/see.lua", "waitable_source": "conds/see.lua",
                "locator": true, "binds": { "desc": "desc", "by": "by" } },
    "in_app": { "cond_source": "conds/in_app.lua", "waitable_source": "conds/in_app.lua",
                "binds": { "package_name": "package_name" } },
    "count":  { "cond_source": "conds/node_target_count.lua", "waitable_source": "conds/node_target_count.lua",
                "locator": true },
    "bb":     { "engine_cond": "Blackboard" }
  },
  "subtree_dir": "bt/subtrees"
}
)json";
}

}  // namespace bt_dsl
