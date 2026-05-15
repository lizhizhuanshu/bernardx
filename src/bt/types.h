#pragma once

#include <cstdint>

enum class NodeStatus : uint8_t {
    kSuccess,
    kFailure,
    kRunning,
};

enum class AbortMode : uint8_t {
    kNone,
    kSelf,
    kLowerPriority,
    kBoth,
};
