#include "resume_sequence.h"

ResumeSequence::ResumeSequence(uint32_t id, std::string name)
    : Composite(id, "ResumeSequence", std::move(name)) {}

NodeStatus ResumeSequence::Tick(Blackboard& bb, BtEventQueue& events) {
    if (!entry_resolved_) {
        // First tick after entry/re-entry: scan top-to-bottom for the first
        // child whose description (decorators) holds. That position is the
        // entry; if none hold, fail.
        size_t entry = children_.size();
        for (size_t i = 0; i < children_.size(); ++i) {
            if (children_[i]->CheckDecorators(bb)) {
                entry = i;
                break;
            }
        }
        if (entry == children_.size()) {
            return NodeStatus::kFailure;
        }
        current_child_index_ = entry;
        entry_resolved_ = true;
    }

    // Run from the remembered position as a plain sequence: Success advances,
    // Failure fails, Running pauses and is remembered. Subsequent ticks resume
    // here without re-scanning; only Reset/OnAborted triggers a fresh entry.
    while (current_child_index_ < children_.size()) {
        auto& child = children_[current_child_index_];
        switch (child->TickAndRecord(bb, events)) {
            case NodeStatus::kRunning:
                return NodeStatus::kRunning;
            case NodeStatus::kFailure:
                if (!child->last_error().empty()) {
                    set_last_error(child->last_error());
                }
                return NodeStatus::kFailure;
            case NodeStatus::kSuccess:
                ++current_child_index_;
                break;
        }
    }

    // Reached the end: succeed, and clear state so the next entry re-scans.
    entry_resolved_ = false;
    current_child_index_ = 0;
    return NodeStatus::kSuccess;
}

void ResumeSequence::Reset() {
    entry_resolved_ = false;
    Composite::Reset();
}

void ResumeSequence::OnAborted() {
    entry_resolved_ = false;
    Composite::OnAborted();
}
