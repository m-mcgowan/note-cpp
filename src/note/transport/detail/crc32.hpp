#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Internal header — use note/transport/serial.hpp (or i2c.hpp) from user code.

namespace note::transport::detail {

// Nibble lookup table for CRC32 (shared by all CRC functions).
inline constexpr uint32_t kCrc32Lut[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

// Half-byte (nibble) CRC32, matching note-c's _crc32().
inline uint32_t crc32(const void* data, size_t len) {
    uint32_t crc = ~uint32_t{0};
    const auto* p = static_cast<const unsigned char*>(data);
    while (len--) {
        crc = kCrc32Lut[(crc ^  *p      ) & 0x0F] ^ (crc >> 4);
        crc = kCrc32Lut[(crc ^ (*p >> 4)) & 0x0F] ^ (crc >> 4);
        ++p;
    }
    return ~crc;
}

// ── Incremental CRC32 ──────────────────────────────────────────────────────
// Allows CRC to be accumulated across multiple write calls.
//
//   uint32_t state = crc32_begin();
//   state = crc32_update(state, chunk1, len1);
//   state = crc32_update(state, chunk2, len2);
//   uint32_t checksum = crc32_finalize(state);
//
// Equivalent to: crc32(chunk1 + chunk2, len1 + len2)

inline uint32_t crc32_begin() { return ~uint32_t{0}; }

inline uint32_t crc32_update(uint32_t state, const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    while (len--) {
        state = kCrc32Lut[(state ^  *p      ) & 0x0F] ^ (state >> 4);
        state = kCrc32Lut[(state ^ (*p >> 4)) & 0x0F] ^ (state >> 4);
        ++p;
    }
    return state;
}

inline uint32_t crc32_finalize(uint32_t state) { return ~state; }

inline void write_hex16(char* out, uint16_t v) {
    static constexpr char h[] = "0123456789ABCDEF";
    out[0] = h[(v >> 12) & 0xF]; out[1] = h[(v >> 8) & 0xF];
    out[2] = h[(v >>  4) & 0xF]; out[3] = h[(v >> 0) & 0xF];
}
inline void write_hex32(char* out, uint32_t v) {
    static constexpr char h[] = "0123456789ABCDEF";
    out[0] = h[(v >> 28) & 0xF]; out[1] = h[(v >> 24) & 0xF];
    out[2] = h[(v >> 20) & 0xF]; out[3] = h[(v >> 16) & 0xF];
    out[4] = h[(v >> 12) & 0xF]; out[5] = h[(v >>  8) & 0xF];
    out[6] = h[(v >>  4) & 0xF]; out[7] = h[(v >>  0) & 0xF];
}
inline uint32_t read_hex(const char* p, int digits) {
    uint32_t v = 0;
    for (int i = 0; i < digits; ++i) {
        char c = p[i];
        uint32_t nib = (c >= '0' && c <= '9') ? uint32_t(c - '0')
                     : (c >= 'A' && c <= 'F') ? uint32_t(c - 'A' + 10)
                     : (c >= 'a' && c <= 'f') ? uint32_t(c - 'a' + 10)
                     : 0;
        v = (v << 4) | nib;
    }
    return v;
}

// CRC field: ,"crc":"SSSS:CCCCCCCC"  — 22 bytes.
inline constexpr size_t kCrcFieldLen = 22;
inline constexpr const char* kCrcKeyTest = "\"crc\":\"";
inline constexpr size_t kCrcKeyTestLen = 7;

/// Overhead bytes the CRC field adds. Wire buffers need this much headroom.
inline constexpr size_t kCrcOverhead = kCrcFieldLen;

/// Append CRC field to JSON in a char buffer.
/// buf[0..len) must be valid JSON ending with '}'.
/// buf must have capacity >= len + kCrcOverhead.
/// Returns new length. Returns len unchanged if no room or invalid JSON.
inline size_t crc_add(char* buf, size_t len, size_t capacity, uint16_t seq) {
    if (len == 0 || buf[len - 1] != '}') return len;
    if (len + kCrcFieldLen > capacity) return len;

    const uint32_t checksum = crc32(buf, len);

    bool is_empty = true;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == ':') { is_empty = false; break; }
    }

    size_t pos = len - 1;  // overwrite closing '}'
    buf[pos++] = is_empty ? ' ' : ',';
    buf[pos++] = '"'; buf[pos++] = 'c'; buf[pos++] = 'r'; buf[pos++] = 'c';
    buf[pos++] = '"'; buf[pos++] = ':'; buf[pos++] = '"';
    write_hex16(buf + pos, seq); pos += 4;
    buf[pos++] = ':';
    write_hex32(buf + pos, checksum); pos += 8;
    buf[pos++] = '"';
    buf[pos++] = '}';
    return pos;
}

/// Validate and strip CRC from a response buffer.
/// buf[0..len) is the response. len is updated to the stripped length.
/// Returns true on CRC error, false on success.
inline bool crc_check_and_strip(char* buf, size_t& len, uint16_t expected_seq,
                                bool& crc_enabled) {
    while (len > 0 && (unsigned char)buf[len - 1] <= ' ')
        --len;

    if (len == 0 || buf[0] != '{' || buf[len - 1] != '}')
        return false;

    if (len < kCrcFieldLen + 2)
        return crc_enabled;

    // Skip CRC check for error responses (matches note-c).
    // Use manual scan instead of strstr to avoid requiring null termination.
    const char err_key[] = "\"err\":\"";
    for (size_t i = 0; i + 6 < len; ++i) {
        if (memcmp(buf + i, err_key, 7) == 0) return false;
    }

    const size_t field_offset = len - 1 - kCrcFieldLen;

    if (memcmp(buf + field_offset + 1, kCrcKeyTest, kCrcKeyTestLen) != 0)
        return crc_enabled;

    crc_enabled = true;

    const char* val = buf + field_offset + 1 + kCrcKeyTestLen;
    const uint16_t actual_seq = static_cast<uint16_t>(read_hex(val, 4));
    const uint32_t actual_crc = static_cast<uint32_t>(read_hex(val + 5, 8));

    // Truncate at CRC field, restore closing '}'.
    len = field_offset;
    buf[len++] = '}';

    const uint32_t expected_crc = crc32(buf, len);
    return (actual_seq != expected_seq || actual_crc != expected_crc);
}

}  // namespace note::transport::detail
