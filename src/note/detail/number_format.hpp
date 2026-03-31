#pragma once

#include <cstddef>
#include <cstdint>

namespace note::detail {

// Write an integer to buf, return number of chars written.
constexpr size_t itoa(char* buf, size_t cap, int32_t value) {
    if (cap == 0) return 0;

    size_t pos = 0;
    uint32_t uv{};
    if (value < 0) {
        buf[pos++] = '-';
        uv = static_cast<uint32_t>(-(value + 1)) + 1;
    } else {
        uv = static_cast<uint32_t>(value);
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

// Write a double to buf. Simple fixed-point: up to 10 decimal digits,
// trailing zeros stripped.
constexpr size_t dtoa(char* buf, size_t cap, double value) {
    if (cap == 0) return 0;
    size_t pos = 0;

    if (value < 0) {
        buf[pos++] = '-';
        value = -value;
    }

    // Integer part.
    auto int_part = static_cast<int64_t>(value);
    double frac = value - static_cast<double>(int_part);

    // Write integer part via itoa logic.
    {
        if (int_part == 0) {
            if (pos < cap) buf[pos++] = '0';
        } else {
            size_t start = pos;
            int64_t v = int_part;
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

} // namespace note::detail
