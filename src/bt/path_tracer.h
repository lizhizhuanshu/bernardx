#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "types.h"

class Node;
struct lua_State;

// Records per-tick "active paths" (root -> active leaf) of a behavior tree so
// post-mortem reports can locate where a tree got stuck or why it switched.
//
// A Parallel node fans out to multiple paths per tick (one chain per child);
// each chain stays a single id-sequence signature, so the model is unchanged.
// Without Parallel, exactly one path is produced per tick and
// sum(path.count) == tick_count(); with Parallel, sum(path.count) ==
// path_occurrences() >= tick_count().
class PathTracer {
public:
    // --- topology mirror (rebuilt on SetRoot) ---
    void SnapshotTopology(Node* root);

    // --- per-run lifecycle ---
    void Reset();  // clear per-run statistics (called on SetRoot and Stop)
    void set_tracing(bool on) { tracing_ = on; }
    bool tracing() const { return tracing_; }

    // --- per-tick sampling; engine drives these in TickOnce order ---
    void BeginTick();  // ++tick_count_; call at the very start of TickOnce
    void OnTickDone(const std::vector<std::vector<Node*>>& active_paths,
                    NodeStatus root_status, int64_t now_ms, const char* note);
    void MarkTerminal(const std::vector<std::vector<uint32_t>>& last_sigs,
                      NodeStatus root_status, const char* note);
    // Convenience: mark the most recent tick's sigset as terminal.
    void MarkCurrentTerminal(NodeStatus root_status, const char* note) {
        MarkTerminal(last_sigset_, root_status, note);
    }

    // --- output ---
    std::string RenderReport() const;
    void BuildLuaTable(lua_State* L) const;

    // --- test accessors ---
    uint64_t tick_count() const { return tick_count_; }
    uint64_t path_occurrences() const { return path_occurrences_; }
    size_t path_count() const { return paths_.size(); }
    size_t switch_count() const { return switches_.size(); }
    int visits(uint32_t id) const {
        auto it = visit_count_.find(id);
        return it != visit_count_.end() ? it->second : 0;
    }
    // Count for a specific signature (0 if absent).
    uint32_t count_for(const std::vector<uint32_t>& sig) const {
        auto it = paths_.find(sig);
        return it != paths_.end() ? it->second.count : 0;
    }
    // Status distributions for a specific signature (for test assertions).
    int leaf_status_count(const std::vector<uint32_t>& sig, NodeStatus s) const {
        auto it = paths_.find(sig);
        return it != paths_.end() ? it->second.leaf_status_counts[StatusIndex(s)] : 0;
    }
    int root_status_count(const std::vector<uint32_t>& sig, NodeStatus s) const {
        auto it = paths_.find(sig);
        return it != paths_.end() ? it->second.root_status_counts[StatusIndex(s)] : 0;
    }

private:
    struct NodeMeta {
        std::string name;
        std::string type;
        uint32_t parent_id = 0;  // 0 = none (root)
        std::vector<uint32_t> child_ids;
    };
    struct PathStats {
        uint32_t count = 0;
        uint64_t first_tick = 0;
        uint64_t last_tick = 0;
        int64_t first_ms = 0;
        int64_t last_ms = 0;
        uint32_t leaf_id = 0;
        bool is_terminal = false;
        int leaf_status_counts[3] = {0, 0, 0};  // indexed by NodeStatus
        int root_status_counts[3] = {0, 0, 0};
    };
    struct SwitchEvent {
        uint64_t tick;
        int64_t ms;
        std::vector<std::vector<uint32_t>> from;  // prior sigset
        std::vector<std::vector<uint32_t>> to;    // new sigset
    };

    static int StatusIndex(NodeStatus s) { return static_cast<int>(s); }
    std::string FormatPath(const std::vector<uint32_t>& sig) const;
    void PushSigsetNames(lua_State* L,
                         const std::vector<std::vector<uint32_t>>& sigset) const;

    std::unordered_map<uint32_t, NodeMeta> nodes_;
    std::map<std::vector<uint32_t>, PathStats> paths_;
    std::unordered_map<uint32_t, int> visit_count_;
    std::unordered_map<uint32_t, size_t> composite_progress_;  // last current_child_index
    std::vector<SwitchEvent> switches_;

    uint64_t tick_count_ = 0;
    int64_t start_ms_ = 0;
    uint64_t path_occurrences_ = 0;
    bool tracing_ = true;
    bool has_terminal_ = false;
    NodeStatus terminal_status_ = NodeStatus::kRunning;
    std::string terminal_note_;
    std::vector<std::vector<uint32_t>> last_sigset_;  // for switch detection
};
