#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Internal header — use note/transport/serial.hpp (or i2c.hpp) from user code.

namespace note::transport::detail {

// Half-byte (nibble) CRC32, matching note-c's _crc32().
// Reference: https://create.stephan-brumme.com/crc32/#half-byte
inline uint32_t crc32(const void* data, size_t len) {
    static constexpr uint32_t lut[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    uint32_t crc = ~uint32_t{0};
    const auto* p = static_cast<const unsigned char*>(data);
    while (len--) {
        crc = lut[(crc ^  *p      ) & 0x0F] ^ (crc >> 4);
        crc = lut[(crc ^ (*p >> 4)) & 0x0F] ^ (crc >> 4);
        ++p;
    }
    return ~crc;
}

// Hex encoding helpers.
inline void write_hex16(char* out, uint16_t v) {
    static constexpr char h[] = "0123456789ABCDEF";
    out[0] = h[(v >> 12) & 0xF];
    out[1] = h[(v >>  8) & 0xF];
    out[2] = h[(v >>  4) & 0xF];
    out[3] = h[(v >>  0) & 0xF];
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

// CRC field: ,"crc":"SSSS:CCCCCCCC"  — 22 bytes (leading comma + field).
// For empty objects (no existing fields) the comma is replaced with a space.
inline constexpr int kCrcFieldLen = 22;  // ,"crc":"SSSS:CCCCCCCC"
inline constexpr const char* kCrcKeyTest = "\"crc\":\"";  // 7 chars
inline constexpr int kCrcKeyTestLen = 7;

// Appends CRC field to json (must end with '}').
// CRC is computed over the original json string.
// Returns modified string with field inserted before closing '}'.
inline std::string crc_add(std::string json, uint16_t seq) {
    if (json.empty() || json.back() != '}') return json;

    const uint32_t checksum = crc32(json.data(), json.size());
    const bool is_empty = (json.find(':') == std::string::npos);

    json.pop_back();  // remove '}'
    json.reserve(json.size() + kCrcFieldLen + 1);

    json += (is_empty ? ' ' : ',');
    json += '"'; json += 'c'; json += 'r'; json += 'c'; json += '"'; json += ':'; json += '"';

    char buf[12];
    write_hex16(buf, seq);   json.append(buf, 4);
    json += ':';
    write_hex32(buf, checksum); json.append(buf, 8);

    json += '"';
    json += '}';
    return json;
}

// Validates and strips the CRC field from response (mutated in place).
//
// Auto-detection: sets crc_enabled=true the first time a CRC field is found.
//
// Returns true (= CRC error) when:
//   - CRC field found but seq or checksum mismatch
//   - crc_enabled=true but no CRC field present in response
// Returns false (= OK) in all other cases.
inline bool crc_check_and_strip(std::string& response, uint16_t expected_seq,
                                bool& crc_enabled) {
    // Trim trailing whitespace (\r\n).
    while (!response.empty() && (unsigned char)response.back() <= ' ')
        response.pop_back();

    // Valid JSON must start with '{' and end with '}'. Skip invalid responses.
    if (response.empty() || response.front() != '{' || response.back() != '}')
        return false;

    // Minimum length is "{}" (2) + CRC field (22) = 24.
    // If too short, it's an error only if we expected CRC.
    if (response.size() < size_t(kCrcFieldLen + 2))
        return crc_enabled;

    // If response contains an "err" field, skip CRC validation (matches note-c).
    if (response.find("\"err\":\"") != std::string::npos) return false;

    // CRC field starts kCrcFieldLen bytes before the end.
    // Layout: ...X,"crc":"SSSS:CCCCCCCC"}
    //              ^ fieldOffset (X is either comma or space)
    const size_t field_offset = response.size() - 1 - kCrcFieldLen;

    // Check for the key pattern at field_offset+1 (skip the comma/space separator).
    if (std::memcmp(response.data() + field_offset + 1,
                    kCrcKeyTest, kCrcKeyTestLen) != 0) {
        // No CRC field found.
        return crc_enabled;  // error only if we expected one
    }

    // CRC field present — firmware supports CRC.
    crc_enabled = true;

    // Value starts after: separator(1) + key(7) = offset+8
    // Format: SSSS:CCCCCCCC"
    const char* val = response.data() + field_offset + 1 + kCrcKeyTestLen;
    const uint16_t actual_seq = static_cast<uint16_t>(read_hex(val, 4));
    const uint32_t actual_crc = static_cast<uint32_t>(read_hex(val + 5, 8));

    // Truncate response at the CRC field to compute checksum over response body.
    response.resize(field_offset);
    response += '}';

    const uint32_t expected_crc = crc32(response.data(), response.size());

    return (actual_seq != expected_seq || actual_crc != expected_crc);
}

}  // namespace note::transport::detail
