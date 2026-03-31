#pragma once
/// @file md5.hpp
/// MD5 hex digest helper — delegates to note::PlatformMd5.
/// Used by card.binary tests to compute the status checksum.

#include <note/md5.hpp>

/// Compute the MD5 hex digest of `data` (len bytes).
inline note::Md5Hex md5_hex(const uint8_t* data, size_t len) {
    note::PlatformMd5 md5;
    return md5.compute(data, len);
}
