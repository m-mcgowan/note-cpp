#pragma once

/// Type-safe duration wrappers for Notecard API fields.
///
/// The Notecard API uses minutes, seconds, and milliseconds across different
/// endpoints. These types prevent accidental unit mixing at compile time while
/// accepting raw integer assignment for convenience.
///
///   using namespace note::literals;
///   req.outbound = 60_mins;    // OK
///   req.outbound = 60;         // OK (implicit int32_t → Minutes)
///   req.outbound = 30_s;       // compile error (Seconds ≠ Minutes)

#include <cstdint>

namespace note {

struct Minutes {
    int32_t count = 0;
    constexpr Minutes() = default;
    constexpr Minutes(int32_t m) : count(m) {}
    constexpr operator int32_t() const { return count; }
};

struct Seconds {
    int32_t count = 0;
    constexpr Seconds() = default;
    constexpr Seconds(int32_t s) : count(s) {}
    constexpr operator int32_t() const { return count; }
};

struct Milliseconds {
    int32_t count = 0;
    constexpr Milliseconds() = default;
    constexpr Milliseconds(int32_t ms) : count(ms) {}
    constexpr operator int32_t() const { return count; }
};

inline namespace literals {

constexpr Minutes operator""_mins(unsigned long long m) {
    return Minutes{static_cast<int32_t>(m)};
}
constexpr Minutes operator""_minutes(unsigned long long m) {
    return Minutes{static_cast<int32_t>(m)};
}
constexpr Seconds operator""_s(unsigned long long s) {
    return Seconds{static_cast<int32_t>(s)};
}
constexpr Seconds operator""_seconds(unsigned long long s) {
    return Seconds{static_cast<int32_t>(s)};
}
constexpr Milliseconds operator""_ms(unsigned long long ms) {
    return Milliseconds{static_cast<int32_t>(ms)};
}

} // inline namespace literals

} // namespace note
