#pragma once

/// Type-safe duration wrappers for Notecard API fields.
///
/// The Notecard API uses different time units across endpoints (minutes,
/// seconds, milliseconds). These types prevent accidental unit mixing at
/// compile time while accepting raw integer assignment for convenience.
///
/// Larger units implicitly convert to smaller ones — write `7_days` where
/// a `Minutes` field is expected and the library does the math. Going the
/// other direction (e.g. Seconds → Minutes) requires explicit construction
/// to prevent accidental precision loss.
///
///   using namespace note::literals;
///   req.outbound = 60_mins;    // OK
///   req.outbound = 2_hours;    // OK (Hours → Minutes, = 120)
///   req.outbound = 7_days;     // OK (Days → Minutes, = 10080)
///   req.outbound = 60;         // OK (implicit int32_t → Minutes)
///   req.outbound = 30_s;       // compile error (Seconds → Minutes not implicit)

#include <cstdint>

namespace note {

// Forward declarations for converting constructors.
struct Hours;
struct Days;

struct Minutes {
    int32_t count = 0;
    constexpr Minutes() = default;
    constexpr Minutes(int32_t m) : count(m) {}
    constexpr Minutes(Hours h);
    constexpr Minutes(Days d);
    constexpr operator int32_t() const { return count; }
};

struct Seconds {
    int32_t count = 0;
    constexpr Seconds() = default;
    constexpr Seconds(int32_t s) : count(s) {}
    constexpr Seconds(Minutes m) : count(m.count * 60) {}
    constexpr Seconds(Hours h);
    constexpr Seconds(Days d);
    constexpr operator int32_t() const { return count; }
};

struct Milliseconds {
    int32_t count = 0;
    constexpr Milliseconds() = default;
    constexpr Milliseconds(int32_t ms) : count(ms) {}
    constexpr Milliseconds(Seconds s) : count(s.count * 1000) {}
    constexpr Milliseconds(Minutes m) : count(m.count * 60000) {}
    constexpr Milliseconds(Hours h);
    constexpr Milliseconds(Days d);
    constexpr operator int32_t() const { return count; }
};

struct Hours {
    int32_t count = 0;
    constexpr Hours() = default;
    constexpr Hours(int32_t h) : count(h) {}
    constexpr Hours(Days d);
    constexpr operator int32_t() const { return count; }
};

struct Days {
    int32_t count = 0;
    constexpr Days() = default;
    constexpr Days(int32_t d) : count(d) {}
    constexpr operator int32_t() const { return count; }
};

// Deferred converting constructors (need complete types).
constexpr Minutes::Minutes(Hours h) : count(h.count * 60) {}
constexpr Minutes::Minutes(Days d) : count(d.count * 1440) {}
constexpr Seconds::Seconds(Hours h) : count(h.count * 3600) {}
constexpr Seconds::Seconds(Days d) : count(d.count * 86400) {}
constexpr Milliseconds::Milliseconds(Hours h) : count(h.count * 3600000) {}
constexpr Milliseconds::Milliseconds(Days d) : count(d.count * 86400000) {}
constexpr Hours::Hours(Days d) : count(d.count * 24) {}

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
constexpr Hours operator""_hours(unsigned long long h) {
    return Hours{static_cast<int32_t>(h)};
}
constexpr Hours operator""_hr(unsigned long long h) {
    return Hours{static_cast<int32_t>(h)};
}
constexpr Hours operator""_h(unsigned long long h) {
    return Hours{static_cast<int32_t>(h)};
}
constexpr Days operator""_days(unsigned long long d) {
    return Days{static_cast<int32_t>(d)};
}
constexpr Days operator""_d(unsigned long long d) {
    return Days{static_cast<int32_t>(d)};
}

} // inline namespace literals

} // namespace note
