#pragma once

#include <chrono>

// Returns milliseconds since steady_clock epoch. Suitable for intervals and
// deadlines; NOT wall-clock time. Monotonic — immune to system clock changes.
inline int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
