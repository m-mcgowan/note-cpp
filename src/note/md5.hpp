#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "types.hpp"

namespace note {

/// Fixed-size MD5 hex digest — 32 lowercase hex characters, no heap allocation.
struct Md5Hex {
    char buf[33]{};  // 32 hex + null terminator

    /// Implicit conversion to string_view for comparisons and field assignment.
    operator string_view() const { return {buf, 32}; }

    bool operator==(string_view other) const { return string_view(*this) == other; }
    bool operator!=(string_view other) const { return string_view(*this) != other; }

    const char* data() const { return buf; }
    static constexpr size_t size() { return 32; }
    bool empty() const { return buf[0] == '\0'; }
};

/// Abstract interface for MD5 computation.
/// Returns the MD5 of `len` raw bytes as a 32-character lowercase hex digest.
///
/// Inject into Notecard to use hardware-accelerated MD5 where available.
/// The library provides SoftwareMd5 (pure C++17, always available) and
/// MbedTlsMd5 (when <mbedtls/md5.h> is detected). PlatformMd5 aliases
/// whichever is best for the current target.
class Md5Provider {
public:
    virtual Md5Hex compute(const uint8_t* data, size_t len) = 0;
    virtual ~Md5Provider() = default;
};


namespace detail {

/// Encode a 16-byte digest into a Md5Hex struct.
inline Md5Hex digest_to_hex(const uint8_t digest[16]) {
    static constexpr char hex[] = "0123456789abcdef";
    Md5Hex out;
    for (size_t i = 0; i < 16; ++i) {
        out.buf[i*2]   = hex[digest[i] >> 4];
        out.buf[i*2+1] = hex[digest[i] & 0xf];
    }
    out.buf[32] = '\0';
    return out;
}

/// RFC 1321 MD5 — pure C++17, no dependencies.
inline Md5Hex md5_hex(const uint8_t* msg, size_t len) {
    // Per-round shift amounts and per-round constants
    static constexpr uint32_t S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
    };
    static constexpr uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
        0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
        0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
        0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
        0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
    };

    auto rotl = [](uint32_t x, uint32_t n) -> uint32_t {
        return (x << n) | (x >> (32u - n));
    };

    uint32_t a0 = 0x67452301u;
    uint32_t b0 = 0xefcdab89u;
    uint32_t c0 = 0x98badcfeu;
    uint32_t d0 = 0x10325476u;

    // Pre-process: pad message to multiple of 512 bits
    size_t orig_len = len;
    size_t padded   = ((len + 8) / 64 + 1) * 64;

    // Process 64-byte chunks — use a local block to handle padding
    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint8_t block[64] = {};
        for (size_t i = 0; i < 64; ++i) {
            size_t pos = chunk + i;
            if      (pos < len)          block[i] = msg[pos];
            else if (pos == len)         block[i] = 0x80;
            else if (pos >= padded - 8)  block[i] = static_cast<uint8_t>(
                                             (orig_len * 8) >> (8 * (pos - (padded - 8))));
        }

        uint32_t M[16];
        for (int i = 0; i < 16; ++i) {
            M[i] = (uint32_t)block[i*4]       | ((uint32_t)block[i*4+1] << 8)
                 | ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);
        }

        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t F, g;
            if      (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;           g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);         g = (7*i)     % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B; B = B + rotl(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }

    // Produce little-endian digest
    uint8_t digest[16];
    uint32_t words[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; ++i) {
        digest[i*4+0] = static_cast<uint8_t>(words[i]);
        digest[i*4+1] = static_cast<uint8_t>(words[i] >> 8);
        digest[i*4+2] = static_cast<uint8_t>(words[i] >> 16);
        digest[i*4+3] = static_cast<uint8_t>(words[i] >> 24);
    }
    return digest_to_hex(digest);
}

} // namespace detail


/// Pure software MD5 — always available, no platform dependencies.
class SoftwareMd5 : public Md5Provider {
public:
    Md5Hex compute(const uint8_t* data, size_t len) override {
        return detail::md5_hex(data, len);
    }
};


// Detect mbedtls MD5 availability. The header may exist (e.g. ESP32 SDK)
// but MD5 can be disabled in the mbedtls config (MBEDTLS_MD5_C not defined).
#if defined(NOTE_USE_MBEDTLS) || (defined(ESP_PLATFORM) && __has_include(<mbedtls/md5.h>))
#include <mbedtls/md5.h>
#endif

#if defined(MBEDTLS_MD5_C) || defined(NOTE_USE_MBEDTLS)

/// mbedtls-backed MD5 — available when mbedtls has MD5 enabled.
/// Typically hardware-accelerated on ESP32 targets.
class MbedTlsMd5 : public Md5Provider {
public:
    Md5Hex compute(const uint8_t* data, size_t len) override {
        uint8_t digest[16];
        mbedtls_md5(data, len, digest);
        return detail::digest_to_hex(digest);
    }
};

using PlatformMd5 = MbedTlsMd5;

#else

using PlatformMd5 = SoftwareMd5;

#endif

} // namespace note
