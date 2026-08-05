#pragma once

#include <cstdint>

enum class NodeStatus : uint8_t {
    kSuccess,
    kFailure,
    kRunning,
};

// UE4/5-style reactive abort mode for a node's guard condition.
//   kNone            — not monitored; condition is only used for Pipeline scan
//                      (and as a one-shot gate at a node's entry via TickAndRecord)
//   kSelf            — while the node runs, monitor; condition Failure aborts self
//   kLowerPriority   — monitor; condition false→true preempts lower-priority siblings
//   kBoth            — Self + LowerPriority
enum class AbortMode : uint8_t {
    kNone,
    kSelf,
    kLowerPriority,
    kBoth,
};
