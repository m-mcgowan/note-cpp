#pragma once
/// @file cobs.hpp
/// COBS (Consistent Overhead Byte Stuffing) encoder/decoder matching note-c.
///
/// note-c's COBS variant uses XOR: the core algorithm eliminates 0x00 bytes
/// (classic COBS), then all output bytes are XOR'd with `eop`. This transforms
/// a zero-free output into an eop-free output. The Notecard firmware uses
/// this same XOR-based scheme, so our encoder/decoder must match exactly.

#include <cstddef>
#include <cstdint>
#include <cstring>

/// The COBS delimiter byte. The Notecard uses newline (0x0A), not zero.
inline constexpr uint8_t COBS_EOP = '\n';

/// Maximum encoded size for a given input length (excluding the EOP byte).
inline constexpr size_t cobs_encoded_size(size_t len) {
    return len + (len / 254) + 1;
}

/// COBS-encode `src` (len bytes) into `dst`, producing output free of `eop`.
/// Matches note-c's _cobsEncode: searches for 0x00 in input, XORs output with eop.
/// Returns the number of encoded bytes written to `dst`.
/// `dst` must be at least cobs_encoded_size(len) bytes.
/// `src` and `dst` must NOT overlap.
inline size_t cobs_encode(const uint8_t* src, size_t len, uint8_t* dst,
                          uint8_t eop = COBS_EOP) {
    uint8_t* start = dst;
    uint8_t code = 1;
    uint8_t* code_ptr = dst++;  // Reserve first byte for code

    size_t remaining = len;
    while (remaining > 0) {
        // Max data bytes before needing a new code byte
        size_t max_bytes = 0xFF - code;
        size_t search_len = (remaining < max_bytes) ? remaining : max_bytes;

        // Find next zero byte in input
        const uint8_t* zero_pos = static_cast<const uint8_t*>(
            memchr(src, 0, search_len));

        size_t chunk_len = zero_pos ? static_cast<size_t>(zero_pos - src) : search_len;

        // Copy data bytes, XOR'd with eop
        if (eop == 0) {
            memcpy(dst, src, chunk_len);
        } else {
            for (size_t i = 0; i < chunk_len; i++) {
                dst[i] = src[i] ^ eop;
            }
        }

        dst += chunk_len;
        src += chunk_len;
        remaining -= chunk_len;
        code += static_cast<uint8_t>(chunk_len);

        if (zero_pos && remaining > 0) {
            // Hit a zero byte: write current code, start new block
            *code_ptr = code ^ eop;
            code = 1;
            code_ptr = dst++;
            src++;          // Skip the zero byte
            remaining--;
        } else if (code == 0xFF) {
            // Hit code limit (254 data bytes): write 0xFF code, start new block
            *code_ptr = code ^ eop;
            code = 1;
            code_ptr = dst++;
        }
    }

    // Write final code byte
    *code_ptr = code ^ eop;

    return static_cast<size_t>(dst - start);
}

/// COBS-decode `src` (len encoded bytes) into `dst`.
/// Matches note-c's _cobsDecode: XORs input with eop, restores 0x00 bytes.
/// Returns the number of decoded bytes written to `dst`.
/// `dst` must be at least `len` bytes (decoded is always <= encoded).
/// `src` and `dst` may overlap (in-place decoding is safe when dst <= src).
inline size_t cobs_decode(const uint8_t* src, size_t len, uint8_t* dst,
                          uint8_t eop = COBS_EOP) {
    const uint8_t* start = dst;
    const uint8_t* end = src + len;
    uint8_t code = 0xFF;  // Special initial value: don't insert zero on first iteration

    while (src < end) {
        // Insert a zero byte unless this is the first iteration
        if (code != 0xFF) {
            *dst++ = 0;
        }

        // Read and un-XOR the code byte
        code = (*src++) ^ eop;

        // code == 0 is the termination marker
        if (code == 0) {
            break;
        }

        // Copy code-1 data bytes, un-XOR'd
        size_t bytes_to_copy = code - 1;
        if (src + bytes_to_copy > end) {
            bytes_to_copy = static_cast<size_t>(end - src);
        }

        if (eop == 0) {
            memmove(dst, src, bytes_to_copy);
        } else {
            for (size_t i = 0; i < bytes_to_copy; i++) {
                dst[i] = src[i] ^ eop;
            }
        }

        dst += bytes_to_copy;
        src += bytes_to_copy;
    }

    return static_cast<size_t>(dst - start);
}
