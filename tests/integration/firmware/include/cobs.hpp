#pragma once
/// @file cobs.hpp
/// Minimal COBS (Consistent Overhead Byte Stuffing) encoder/decoder
/// for the card.binary integration tests.

#include <cstddef>
#include <cstdint>

/// Maximum encoded size for a given input length.
inline constexpr size_t cobs_encoded_size(size_t len) {
    return len + (len / 254) + 1;
}

/// COBS-encode `src` (len bytes) into `dst`.
/// Returns the number of encoded bytes written to `dst`.
/// `dst` must be at least cobs_encoded_size(len) bytes.
inline size_t cobs_encode(const uint8_t* src, size_t len, uint8_t* dst) {
    uint8_t* start = dst;
    const uint8_t* end = src + len;
    uint8_t* code_ptr = dst++;
    uint8_t code = 1;

    while (src < end) {
        if (*src == 0) {
            *code_ptr = code;
            code_ptr = dst++;
            code = 1;
        } else {
            *dst++ = *src;
            code++;
            if (code == 0xFF) {
                *code_ptr = code;
                code_ptr = dst++;
                code = 1;
            }
        }
        src++;
    }
    *code_ptr = code;
    return static_cast<size_t>(dst - start);
}

/// COBS-decode `src` (len encoded bytes) into `dst`.
/// Returns the number of decoded bytes written to `dst`.
/// `dst` must be at least `len` bytes (decoded is always <= encoded).
inline size_t cobs_decode(const uint8_t* src, size_t len, uint8_t* dst) {
    const uint8_t* end = src + len;
    uint8_t* out = dst;

    while (src < end) {
        uint8_t code = *src++;
        for (uint8_t i = 1; i < code && src < end; i++) {
            *out++ = *src++;
        }
        if (code < 0xFF && src < end) {
            *out++ = 0;
        }
    }
    // Remove trailing zero if present (COBS adds one)
    if (out > dst && *(out - 1) == 0) {
        out--;
    }
    return static_cast<size_t>(out - dst);
}
