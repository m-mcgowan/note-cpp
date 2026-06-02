#pragma once

#include <note/note_config.hpp>
#include <note/types.hpp>

#include <cstddef>
#include <cstdint>

#if !NOTE_INT32_MATH
#  include <cstdio>
#endif

namespace note::detail {

/// @file number_format.hpp
/// Two double formatters:
///
///   `dtoa` — constexpr, fixed-point. Faithful but verbose
///     (3.3 → "3.2999999999"). Required by `JsonBuf<N>` so JSON can be
///     built at compile time. Also the only formatter available on AVR,
///     where `snprintf("%g", …)` pulls in ~1.5 KB of float-printf.
///
///   `dtoa_shortest` — runtime, shortest round-trip. 3.3 → "3.3".
///     Used by wire emitters (StreamingJsonBuilder, StaticJsonBuilder)
///     where shorter is better and the snprintf cost is acceptable.
///     Falls back to `dtoa` on AVR (NOTE_INT32_MATH).

// Write an integer to buf, return number of chars written.
constexpr size_t itoa(char* buf, size_t cap, json_int_t value) {
    if (cap == 0) return 0;

    size_t pos = 0;
    uint64_t uv{};
    if (value < 0) {
        buf[pos++] = '-';
        uv = static_cast<uint64_t>(-(value + 1)) + 1;
    } else {
        uv = static_cast<uint64_t>(value);
    }

    // Write digits in reverse, then flip.
    size_t start = pos;
    do {
        if (pos >= cap) return pos;
        buf[pos++] = '0' + static_cast<char>(uv % 10);
        uv /= 10;
    } while (uv > 0);

    // Reverse the digits.
    for (size_t i = start, j = pos - 1; i < j; ++i, --j) {
        char tmp = buf[i];
        buf[i] = buf[j];
        buf[j] = tmp;
    }
    return pos;
}

// Constexpr fixed-point conversion. Up to 10 decimal digits, trailing
// zeros stripped. Faithful but not shortest — 3.3 emits "3.2999999999"
// because the double's true value sits just below 3.3 and the loop
// never reaches a clean zero. Used by `JsonBuf<N>` (constexpr literal
// JSON) and by AVR builds (where snprintf-float would add ~1.5 KB).
// Runtime wire emitters should use `dtoa_shortest` instead.
constexpr size_t dtoa(char* buf, size_t cap, double value) {
    if (cap == 0) return 0;
    size_t pos = 0;

    if (value < 0) {
        buf[pos++] = '-';
        value = -value;
    }

    // Integer part. NOTE_INT32_MATH uses int32_t to avoid pulling in 64-bit
    // soft-math on platforms without hardware 64-bit ops (~286 bytes on AVR).
    // Values above INT32_MAX are truncated. Full builds use int64_t.
#if NOTE_INT32_MATH
    auto int_part = static_cast<int32_t>(value);
#else
    auto int_part = static_cast<int64_t>(value);
#endif
    double frac = value - static_cast<double>(int_part);

    // Write integer part via itoa logic.
    {
        if (int_part == 0) {
            if (pos < cap) buf[pos++] = '0';
        } else {
            size_t start = pos;
            decltype(int_part) v = int_part;
            while (v > 0 && pos < cap) {
                buf[pos++] = '0' + static_cast<char>(v % 10);
                v /= 10;
            }
            for (size_t i = start, j = pos - 1; i < j; ++i, --j) {
                char tmp = buf[i];
                buf[i] = buf[j];
                buf[j] = tmp;
            }
        }
    }

    // Fractional part -- up to 10 digits, strip trailing zeros.
    constexpr int max_frac_digits = 10;
    if (frac > 0.0) {
        if (pos < cap) buf[pos++] = '.';
        size_t frac_start = pos;
        double f = frac;
        for (int d = 0; d < max_frac_digits && pos < cap; ++d) {
            f *= 10.0;
            int digit = static_cast<int>(f);
            buf[pos++] = '0' + static_cast<char>(digit);
            f -= digit;
            if (f < 1e-10) break;
        }
        while (pos > frac_start + 1 && buf[pos - 1] == '0') --pos;
    }

    return pos;
}

// Runtime shortest-round-trip formatter. Walks precisions 1..17 with
// %.*g and accepts the first that round-trips via strtod. 17 sig digits
// is enough to uniquely identify any IEEE-754 double, so the loop
// terminates. Falls back to %.17g if nothing round-tripped (defensive —
// shouldn't happen for finite values).
//
// On AVR (NOTE_INT32_MATH=1), forwards to constexpr `dtoa` — the
// snprintf/sscanf machinery adds ~1.5 KB of float-printf code that
// AVR-class targets don't want.
inline size_t dtoa_shortest(char* buf, size_t cap, double value) {
#if NOTE_INT32_MATH
    return dtoa(buf, cap, value);
#else
    if (cap == 0) return 0;

    char tmp[32];
    for (int prec = 1; prec <= 17; ++prec) {
        int n = std::snprintf(tmp, sizeof(tmp), "%.*g", prec, value);
        if (n <= 0) break;
        double parsed = 0.0;
        if (std::sscanf(tmp, "%lf", &parsed) == 1 && parsed == value) {
            size_t copy = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap;
            for (size_t i = 0; i < copy; ++i) buf[i] = tmp[i];
            return copy;
        }
    }

    int n = std::snprintf(tmp, sizeof(tmp), "%.17g", value);
    if (n <= 0) return 0;
    size_t copy = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap;
    for (size_t i = 0; i < copy; ++i) buf[i] = tmp[i];
    return copy;
#endif
}

} // namespace note::detail
