#include "path_tracer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "composite.h"
#include "lua_value_utils.h"
#include "node.h"
#include "single_child_node.h"
#include "time_utils.h"

namespace {

const char* StatusString(NodeStatus s) {
    switch (s) {
        case NodeStatus::kSuccess: return "success";
        case NodeStatus::kFailure: return "failure";
        case NodeStatus::kRunning: return "running";
    }
    return "?";
}

char StatusChar(NodeStatus s) {
    switch (s) {
        case NodeStatus::kSuccess: return 'S';
        case NodeStatus::kFailure: return 'F';
        case NodeStatus::kRunning: return 'R';
    }
    return '?';
}

// Builds a {success=, failure=, running=} table on top of the stack.
void PushStatusTable(lua_State* L, const int cnt[3]) {
    lua_newtable(L);
    lua_pushinteger(L, cnt[static_cast<int>(NodeStatus::kSuccess)]);
    lua_setfield(L, -2, "success");
    lua_pushinteger(L, cnt[static_cast<int>(NodeStatus::kFailure)]);
    lua_setfield(L, -2, "failure");
    lua_pushinteger(L, cnt[static_cast<int>(NodeStatus::kRunning)]);
    lua_setfield(L, -2, "running");
}

std::string Bar(int visit, int max_visit) {
    if (max_visit <= 0) return "";
    int len = (visit * 10 + max_visit / 2) / max_visit;  // rounded, 0..10
    if (len < 0) len = 0;
    if (len > 10) len = 10;
    std::string b;
    for (int i = 0; i < len; ++i) b += "\xe2\x96\x87";  // U+2587 FULL BLOCK
    for (int i = len; i < 10; ++i) b += ' ';
    return b;
}

}  // namespace

void PathTracer::SnapshotTopology(Node* root) {
    nodes_.clear();
    composite_progress_.clear();
    if (!root) return;

    // Iterative DFS; each node records its parent id.
    struct Frame {
        Node* node;
        uint32_t parent_id;
    };
    std::vector<Frame> stack;
    stack.push_back({root, 0});

    while (!stack.empty()) {
        auto [node, parent_id] = stack.back();
        stack.pop_back();
        if (!node) continue;

        const uint32_t id = node->id();
        NodeMeta meta;
        meta.name = node->name();
        meta.type = node->type();
        meta.parent_id = parent_id;

        if (auto* comp = dynamic_cast<Composite*>(node)) {
            for (const auto& child : comp->children()) {
                if (child) {
                    meta.child_ids.push_back(child->id());
                    stack.push_back({child.get(), id});
                }
            }
        } else if (auto* single = dynamic_cast<SingleChildNode*>(node)) {
            if (single->child()) {
                meta.child_ids.push_back(single->child()->id());
                stack.push_back({single->child(), id});
            }
        }
        nodes_[id] = std::move(meta);
    }
}

void PathTracer::Reset() {
    paths_.clear();
    visit_count_.clear();
    composite_progress_.clear();
    switches_.clear();
    tick_count_ = 0;
    path_occurrences_ = 0;
    has_terminal_ = false;
    terminal_note_.clear();
    terminal_status_ = NodeStatus::kRunning;
    last_sigset_.clear();
    start_ms_ = NowMs();
}

void PathTracer::BeginTick() {
    if (tracing_) ++tick_count_;
}

void PathTracer::OnTickDone(const std::vector<std::vector<Node*>>& active_paths,
                            NodeStatus root_status, int64_t now_ms,
                            const char* note) {
    if (!tracing_) return;

    terminal_status_ = root_status;
    if (note) terminal_note_ = note; else terminal_note_.clear();

    std::vector<std::vector<uint32_t>> sigset;
    std::set<uint32_t> visited_this_tick;

    for (const auto& path : active_paths) {
        if (path.empty()) continue;
        std::vector<uint32_t> sig;
        sig.reserve(path.size());
        for (Node* n : path) {
            if (!n) continue;
            const uint32_t id = n->id();
            sig.push_back(id);
            visited_this_tick.insert(id);
            if (auto* comp = dynamic_cast<Composite*>(n)) {
                composite_progress_[id] = comp->current_child_index();
            }
        }
        if (sig.empty()) continue;

        const uint32_t leaf_id = sig.back();
        const NodeStatus leaf_status =
            path.back() ? path.back()->last_tick_status() : NodeStatus::kRunning;

        auto& stats = paths_[sig];
        const bool first = (stats.count == 0);
        stats.count++;
        stats.last_tick = tick_count_;
        stats.last_ms = now_ms;
        if (first) {
            stats.first_tick = tick_count_;
            stats.first_ms = now_ms;
            stats.leaf_id = leaf_id;
        }
        stats.leaf_status_counts[StatusIndex(leaf_status)]++;
        stats.root_status_counts[StatusIndex(root_status)]++;
        ++path_occurrences_;

        sigset.push_back(std::move(sig));
    }

    for (uint32_t id : visited_this_tick) {
        visit_count_[id]++;
    }

    // Switch detection: compare this tick's sorted unique sigset to last.
    std::sort(sigset.begin(), sigset.end());
    sigset.erase(std::unique(sigset.begin(), sigset.end()), sigset.end());
    if (!last_sigset_.empty() && sigset != last_sigset_) {
        switches_.push_back({tick_count_, now_ms, last_sigset_, sigset});
    }
    last_sigset_ = std::move(sigset);
}

void PathTracer::MarkTerminal(const std::vector<std::vector<uint32_t>>& last_sigs,
                              NodeStatus root_status, const char* note) {
    if (!tracing_) return;
    has_terminal_ = true;
    terminal_status_ = root_status;
    if (note) terminal_note_ = note;
    for (const auto& sig : last_sigs) {
        auto it = paths_.find(sig);
        if (it != paths_.end()) {
            it->second.is_terminal = true;
        }
    }
}

std::string PathTracer::FormatPath(const std::vector<uint32_t>& sig) const {
    std::string s;
    for (size_t i = 0; i < sig.size(); ++i) {
        if (i) s += " \xe2\x96\xb8 ";  // U+25B8 "▸"
        auto it = nodes_.find(sig[i]);
        if (it != nodes_.end()) {
            s += it->second.name;
        } else {
            s += "#" + std::to_string(sig[i]);
        }
    }
    return s;
}

std::string PathTracer::RenderReport() const {
    std::string r;
    r.reserve(4096);
    auto add = [&](const std::string& s) { r += s; r += '\n'; };

    const std::string term = has_terminal_ ? StatusString(terminal_status_) : "n/a";
    add("===== 行为树路径报告 =====");
    add("ticks=" + std::to_string(tick_count_) +
        "  path_occurrences=" + std::to_string(path_occurrences_) +
        "  paths=" + std::to_string(paths_.size()) +
        "  switches=" + std::to_string(switches_.size()) +
        "  末态=" + term +
        (has_terminal_ && !terminal_note_.empty() ? (" (" + terminal_note_ + ")") : ""));

    if (paths_.empty()) {
        add("(无路径数据:trace_paths=false 或树未运行)");
        return r;
    }

    // --- A: path heat list ---
    add("");
    add("▸ A 路径热度列表 (按 count 降序)");
    std::vector<const std::pair<const std::vector<uint32_t>, PathStats>*> sorted;
    for (const auto& kv : paths_) sorted.push_back(&kv);
    std::sort(sorted.begin(), sorted.end(),
              [](auto* a, auto* b) { return a->second.count > b->second.count; });

    const size_t TOPK = 20;
    const uint32_t FOLD_THRESHOLD = 2;  // paths with count <= this are "transient"
    size_t shown = 0;
    uint64_t folded_count = 0;
    size_t folded_paths = 0;
    for (const auto* kv : sorted) {
        if (kv->second.count <= FOLD_THRESHOLD && shown >= TOPK) {
            folded_count += kv->second.count;
            ++folded_paths;
            continue;
        }
        if (shown >= TOPK) {
            folded_count += kv->second.count;
            ++folded_paths;
            continue;
        }
        ++shown;
        const auto& sig = kv->first;
        const auto& st = kv->second;
        const uint64_t denom = path_occurrences_ ? path_occurrences_ : 1;
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%3llu%%",
                      static_cast<unsigned long long>(st.count * 100 / denom));
        char range[64];
        std::snprintf(range, sizeof(range), "t[%llu..%llu]",
                      static_cast<unsigned long long>(st.first_tick),
                      static_cast<unsigned long long>(st.last_tick));
        std::string line = " " + std::to_string(st.count) + "  " + pct + "  " + range +
                           "  " + FormatPath(sig);
        if (st.is_terminal) line += "  ✱末态";
        // leaf status summary
        std::string ls;
        const char* names[3] = {"success", "failure", "running"};
        for (int i = 0; i < 3; ++i) {
            if (st.leaf_status_counts[i]) {
                ls += std::string(ls.empty() ? "" : " ") +
                      names[i] + "×" + std::to_string(st.leaf_status_counts[i]);
            }
        }
        if (!ls.empty()) line += "   叶:" + ls;
        add(line);
    }
    if (folded_paths) {
        add(" — 折叠 " + std::to_string(folded_paths) + " 条瞬态路径 (count≤" +
            std::to_string(FOLD_THRESHOLD) + "), 合计 " + std::to_string(folded_count));
    }

    // --- B: tree heat map ---
    add("");
    add("▸ B 树形热力图 (节点被经过的 tick 数)");
    // find root (parent_id == 0)
    uint32_t root_id = 0;
    int max_visits = 1;
    for (const auto& [id, m] : nodes_) {
        if (m.parent_id == 0) { root_id = id; break; }
    }
    for (const auto& [id, v] : visit_count_) {
        if (v > max_visits) max_visits = v;
    }
    if (root_id != 0) {
        // iterative DFS preserving child order
        struct F { uint32_t id; int depth; };
        std::vector<F> stk;
        stk.push_back({root_id, 0});
        while (!stk.empty()) {
            F fr = stk.back();
            stk.pop_back();
            auto it = nodes_.find(fr.id);
            if (it == nodes_.end()) continue;
            const auto& m = it->second;
            const int v = visits(fr.id);
            std::string indent(fr.depth * 2, ' ');
            std::string line = indent + m.name + " [" + m.type + "]" +
                               std::string(std::max<int>(0, 28 - (int)(indent.size() + m.name.size() + m.type.size() + 3)), ' ');
            line += std::to_string(v) + " " + Bar(v, max_visits);
            if (m.type == "Parallel") {
                line += "  ∥parallel(" + std::to_string(m.child_ids.size()) + ")";
            } else if (!m.child_ids.empty()) {
                auto pit = composite_progress_.find(fr.id);
                if (pit != composite_progress_.end()) {
                    line += "  [" + std::to_string(pit->second) + "/" +
                            std::to_string(m.child_ids.size()) + "]";
                }
            }
            add(line);
            // push children in reverse so first child renders first
            for (auto rit = m.child_ids.rbegin(); rit != m.child_ids.rend(); ++rit) {
                stk.push_back({*rit, fr.depth + 1});
            }
        }
    }

    // --- C: switch timeline ---
    add("");
    add("▸ C 路径切换时间线 (仅切换事件)");
    if (switches_.empty()) {
        add(" (无切换)");
    } else {
        const size_t LASTK = 50;
        size_t start = switches_.size() > LASTK ? switches_.size() - LASTK : 0;
        for (size_t i = start; i < switches_.size(); ++i) {
            const auto& sw = switches_[i];
            std::string line = " t=" + std::to_string(sw.tick) + "  ";
            if (!sw.from.empty()) {
                line += FormatPath(sw.from.front());
                if (sw.from.size() > 1) line += "(+" + std::to_string(sw.from.size() - 1) + ")";
            }
            line += "  →  ";
            if (!sw.to.empty()) {
                line += FormatPath(sw.to.front());
                if (sw.to.size() > 1) line += "(+" + std::to_string(sw.to.size() - 1) + ")";
            }
            add(line);
        }
        if (start > 0) {
            add(" (仅显示最近 " + std::to_string(switches_.size() - start) +
                " 次切换, 共 " + std::to_string(switches_.size()) + " 次)");
        }
    }

    add("=========================");
    return r;
}

void PathTracer::PushSigsetNames(lua_State* L,
                                 const std::vector<std::vector<uint32_t>>& sigset) const {
    lua_newtable(L);
    int i = 1;
    for (const auto& sig : sigset) {
        lua_pushstring(L, FormatPath(sig).c_str());
        lua_seti(L, -2, i++);
    }
}

void PathTracer::BuildLuaTable(lua_State* L) const {
    lua_newtable(L);

    lua_pushinteger(L, static_cast<lua_Integer>(tick_count_));
    lua_setfield(L, -2, "total_ticks");
    lua_pushinteger(L, static_cast<lua_Integer>(path_occurrences_));
    lua_setfield(L, -2, "path_occurrences");
    lua_pushinteger(L, static_cast<lua_Integer>(start_ms_));
    lua_setfield(L, -2, "start_ms");
    lua_pushstring(L, StatusString(terminal_status_));
    lua_setfield(L, -2, "terminal");
    lua_pushboolean(L, has_terminal_);
    lua_setfield(L, -2, "has_terminal");
    PushLuaValue(L, terminal_note_);
    lua_setfield(L, -2, "terminal_note");
    lua_pushboolean(L, tracing_);
    lua_setfield(L, -2, "tracing");

    // paths
    lua_newtable(L);
    int pi = 1;
    for (const auto& [sig, st] : paths_) {
        lua_newtable(L);
        lua_newtable(L);
        for (size_t k = 0; k < sig.size(); ++k) {
            lua_pushinteger(L, sig[k]);
            lua_seti(L, -2, static_cast<lua_Integer>(k + 1));
        }
        lua_setfield(L, -2, "sig_ids");
        lua_newtable(L);
        for (size_t k = 0; k < sig.size(); ++k) {
            auto it = nodes_.find(sig[k]);
            std::string nm = it != nodes_.end()
                                 ? it->second.name
                                 : ("#" + std::to_string(sig[k]));
            lua_pushstring(L, nm.c_str());
            lua_seti(L, -2, static_cast<lua_Integer>(k + 1));
        }
        lua_setfield(L, -2, "names");
        lua_pushinteger(L, static_cast<lua_Integer>(st.count));
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, static_cast<lua_Integer>(st.first_tick));
        lua_setfield(L, -2, "first_tick");
        lua_pushinteger(L, static_cast<lua_Integer>(st.last_tick));
        lua_setfield(L, -2, "last_tick");
        lua_pushinteger(L, static_cast<lua_Integer>(st.first_ms));
        lua_setfield(L, -2, "first_ms");
        lua_pushinteger(L, static_cast<lua_Integer>(st.last_ms));
        lua_setfield(L, -2, "last_ms");
        lua_pushinteger(L, static_cast<lua_Integer>(st.leaf_id));
        lua_setfield(L, -2, "leaf_id");
        lua_pushboolean(L, st.is_terminal);
        lua_setfield(L, -2, "is_terminal");
        PushStatusTable(L, st.leaf_status_counts);
        lua_setfield(L, -2, "leaf_status");
        PushStatusTable(L, st.root_status_counts);
        lua_setfield(L, -2, "root_status");
        lua_seti(L, -2, pi++);
    }
    lua_setfield(L, -2, "paths");

    // nodes
    lua_newtable(L);
    int ni = 1;
    for (const auto& [id, m] : nodes_) {
        lua_newtable(L);
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        lua_setfield(L, -2, "id");
        lua_pushstring(L, m.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, m.type.c_str());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, static_cast<lua_Integer>(m.parent_id));
        lua_setfield(L, -2, "parent_id");
        lua_newtable(L);
        for (size_t k = 0; k < m.child_ids.size(); ++k) {
            lua_pushinteger(L, m.child_ids[k]);
            lua_seti(L, -2, static_cast<lua_Integer>(k + 1));
        }
        lua_setfield(L, -2, "child_ids");
        lua_pushinteger(L, static_cast<lua_Integer>(visits(id)));
        lua_setfield(L, -2, "visits");
        auto pit = composite_progress_.find(id);
        if (pit != composite_progress_.end()) {
            lua_pushinteger(L, static_cast<lua_Integer>(pit->second));
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "composite_progress");
        lua_seti(L, -2, ni++);
    }
    lua_setfield(L, -2, "nodes");

    // switches
    lua_newtable(L);
    int si = 1;
    for (const auto& sw : switches_) {
        lua_newtable(L);
        lua_pushinteger(L, static_cast<lua_Integer>(sw.tick));
        lua_setfield(L, -2, "tick");
        lua_pushinteger(L, static_cast<lua_Integer>(sw.ms));
        lua_setfield(L, -2, "ms");
        PushSigsetNames(L, sw.from);
        lua_setfield(L, -2, "from");
        PushSigsetNames(L, sw.to);
        lua_setfield(L, -2, "to");
        lua_seti(L, -2, si++);
    }
    lua_setfield(L, -2, "switches");
}
