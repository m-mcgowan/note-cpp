#pragma once
/// @file md5.hpp
/// MD5 hex digest helper using mbedtls (available on ESP32).
/// Used by card.binary tests to compute the status checksum.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <mbedtls/md5.h>

/// Compute the MD5 hex digest of `data` (len bytes).
inline std::string md5_hex(const uint8_t* data, size_t len) {
    uint8_t hash[16];
    mbedtls_md5(data, len, hash);

    char hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    return std::string(hex, 32);
}
