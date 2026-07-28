#pragma once

#include "composite.h"

// ResumeSequence is a condition-gated sequence with memory.
//
// On the first tick after entering (or re-entering after an abort), it scans
// children top-to-bottom and picks the first whose decorators (the
// "description"/condition) hold as the entry point; if none hold it fails.
// From that entry it then runs as a normal Sequence with memory: Success
// advances to the next child, Failure fails the node, Running pauses and is
// remembered. Subsequent ticks resume from the remembered position without
// re-scanning -- only an abort/Reset triggers a fresh entry scan.
//
// This is "first tick finds the position, later ticks run from that position,
// the next first tick re-scans top-to-bottom": an interrupt handled by an
// ancestor (e.g. a popup dismissed by an upper Selector) aborts this node, so
// the re-entry scan skips already-finished steps (whose descriptions no
// longer hold) and resumes at the current step.
class ResumeSequence : public Composite {
public:
    ResumeSequence(uint32_t id, std::string name);

    NodeStatus Tick(Blackboard& bb, BtEventQueue& events) override;
    void Reset() override;
    void OnAborted() override;

private:
    bool entry_resolved_ = false;
};
