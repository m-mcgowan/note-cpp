#pragma once

// Monotonic clock + sleep helpers used by the POSIX HALs.
//
// Both PosixSerialHal and LinuxI2cHal need the same millis/delay behaviour:
// a monotonic ms counter (immune to system time changes) and a ms-resolution
// sleep. Defined once here, consumed by each HAL's HAL-interface overrides.

#if defined(__unix__) || defined(__APPLE__)

#include <time.h>

#include <cstdint>

namespace note::posix::clock {

// Monotonic millisecond counter. Wraps after ~49 days, matching the
// Arduino ::millis() contract used throughout note-cpp.
inline uint32_t millis() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t sec_ms  = static_cast<uint64_t>(ts.tv_sec)  * 1000ULL;
    const uint64_t nsec_ms = static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    return static_cast<uint32_t>(sec_ms + nsec_ms);
}

// Sleep for approximately `ms` milliseconds.
inline void sleep_ms(uint32_t ms) {
    struct timespec ts{
        static_cast<time_t>(ms / 1000),
        static_cast<long>((ms % 1000) * 1000000L)
    };
    ::nanosleep(&ts, nullptr);
}

}  // namespace note::posix::clock

#endif  // __unix__ || __APPLE__
