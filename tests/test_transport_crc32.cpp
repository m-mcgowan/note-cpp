// Tests for note::link::detail CRC32 implementation.
//
// Ported from note-c test/src/_crcAdd_test.cpp and _crcError_test.cpp.
// Reference commits:
//   - _crcError_test.cpp (initial): includes memcmp size regression test
//   - 13f8581 "fix: Missing CRC bug": additional tests for too-short responses
//     and updated _crcError ordering
//
// When note-c's CRC tests change, review the diff against these files and
// update accordingly.  Key mapping:
//   note-c _crcAdd()     → note-cpp crc_add()
//   note-c _crcError()   → note-cpp crc_check_and_strip() (also strips field)
//   notecardFirmwareSupportsCrc → the bool& crc_enabled parameter

#include <doctest.h>
#include <string>

#include <note/link/detail/crc32.hpp>

using namespace note::link::detail;

// Helpers for char-buffer CRC API
namespace {
inline std::string add_crc(const char* json, uint16_t seq) {
    char buf[512];
    size_t len = strlen(json);
    memcpy(buf, json, len);
    size_t new_len = crc_add(buf, len, sizeof(buf), seq);
    return std::string(buf, new_len);
}
inline std::string add_crc(const std::string& json, uint16_t seq) {
    return add_crc(json.c_str(), seq);
}
inline bool check_crc(std::string& response, uint16_t seq, bool& flag) {
    size_t len = response.size();
    // Need writable buffer
    char buf[512];
    memcpy(buf, response.data(), len);
    bool err = crc_check_and_strip(buf, len, seq, flag);
    response.assign(buf, len);
    return err;
}
} // namespace

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// crc32 — standard test vectors
// ---------------------------------------------------------------------------

TEST_CASE("crc32 empty input is 0x00000000") {
    REQUIRE(crc32("", 0) == 0x00000000u);
}

TEST_CASE("crc32 standard check value for '123456789'") {
    // CRC32/ISO-HDLC standard check value
    REQUIRE(crc32("123456789", 9) == 0xCBF43926u);
}

TEST_CASE("crc32 single byte 0x00") {
    const uint8_t b = 0x00;
    REQUIRE(crc32(&b, 1) == 0xD202EF8Du);
}

TEST_CASE("crc32 single byte 0xFF") {
    const uint8_t b = 0xFF;
    REQUIRE(crc32(&b, 1) == 0xFF000000u);
}

// ---------------------------------------------------------------------------
// crc_add — ported from note-c _crcAdd_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("crc_add empty string returns unchanged") {
    // note-c: _crcAdd("", seqNo) == NULL (malloc would succeed but logic bails)
    // Our version: if json is empty or doesn't end with '}', return as-is.
    std::string result = add_crc("", 1);
    REQUIRE(result.empty());
}

TEST_CASE("crc_add empty object produces space-separated CRC field") {
    // note-c: _crcAdd("{}", 1) == "{ \"crc\":\"0001:A3A6BF43\"}"
    // Verified against note-c's _crc32 implementation.
    std::string result = add_crc("{}", 1);
    REQUIRE(result == "{ \"crc\":\"0001:A3A6BF43\"}");
}

TEST_CASE("crc_add valid JSON produces comma-separated CRC field") {
    // note-c: _crcAdd("{\"req\": \"hub.sync\"}", 1) == "{\"req\": \"hub.sync\",\"crc\":\"0001:DF2B9115\"}"
    std::string result = add_crc("{\"req\": \"hub.sync\"}", 1);
    REQUIRE(result == "{\"req\": \"hub.sync\",\"crc\":\"0001:DF2B9115\"}");
}

TEST_CASE("crc_add input without closing brace is returned unchanged") {
    // note-c: invalid JSON (no closing brace) → returns NULL
    std::string input = "{\"req\":";
    std::string result = add_crc(input, 1);
    REQUIRE(result == input);
}

TEST_CASE("crc_add CRC field format is SSSS:CCCCCCCC") {
    std::string out = add_crc("{\"req\":\"hub.set\"}", 0xAB12);
    auto pos = out.find("\"crc\":\"");
    REQUIRE(pos != std::string::npos);
    const char* val = out.data() + pos + 7;  // skip "crc":"
    // SSSS is 4 hex chars
    REQUIRE(val[0] == 'A');
    REQUIRE(val[1] == 'B');
    REQUIRE(val[2] == '1');
    REQUIRE(val[3] == '2');
    REQUIRE(val[4] == ':');
    // Then 8 hex chars + closing quote
    REQUIRE(val[13] == '"');
}

TEST_CASE("crc_add total length increase is exactly kCrcFieldLen") {
    std::string json = R"({"req":"hub.set"})";
    std::string out  = add_crc(json, 0x0001);
    REQUIRE(out.size() == json.size() + kCrcFieldLen);
}

// ---------------------------------------------------------------------------
// crc_check_and_strip — ported from note-c _crcError_test.cpp
// "Does NOT support CRC" section (crc_enabled = false)
// ---------------------------------------------------------------------------

TEST_CASE("crc_check_and_strip: CRC not enabled, empty string — no error") {
    // note-c: _crcError("", seqNo) == false when !notecardFirmwareSupportsCrc
    bool flag = false;
    std::string response = "";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(!flag);
}

TEST_CASE("crc_check_and_strip: CRC not enabled, invalid JSON (missing closing brace) — no error") {
    // note-c: _crcError("{\"req\":\"hub.sync\",\"crc\":\"0009:10BAC79A\"", ...) == false
    bool flag = false;
    std::string response = "{\"req\":\"hub.sync\",\"crc\":\"0009:10BAC79A\"";  // no closing brace
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(!flag);
}

TEST_CASE("crc_check_and_strip: CRC not enabled, error response — no error") {
    // note-c: err field present → skip CRC check
    bool flag = false;
    std::string response = R"({"err":"cannot interpret JSON: bool being placed into a non-bool field {io}"})";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(!flag);
}

TEST_CASE("crc_check_and_strip: CRC not enabled, no CRC field, short response — no error") {
    // note-c: no CRC field, notecardFirmwareSupportsCrc=false → no error
    bool flag = false;
    std::string response = R"({"req": "hub.sync"})";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(!flag);
}

TEST_CASE("crc_check_and_strip: CRC not enabled, CRC not at tail — no error, flag stays false") {
    // note-c: CRC field embedded but not at tail → not detected → no error when !enabled
    bool flag = false;
    std::string response = R"({"crc":"0009:10BAC79A","req": "hub.sync"})";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(!flag);  // crc_enabled must NOT be set when CRC is not at tail
}

TEST_CASE("crc_check_and_strip: hub.status regression — quote at CRC check offset, no error, flag stays false") {
    // note-c regression: {"connected":false,"status":"connecting"} is 41 bytes.
    // field_offset = 41-1-22 = 18. Position 19 is '"' (start of "status").
    // With the memcmp size bug (1 byte instead of 7), '"' matched '"' in
    // kCrcKeyTest, permanently flipping notecardFirmwareSupportsCrc to true.
    // Our 7-byte memcmp correctly rejects this.
    bool flag = false;
    std::string response = R"({"connected":false,"status":"connecting"})";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(!flag);  // flag must NOT be set by a false CRC field detection
}

TEST_CASE("crc_check_and_strip: CRC not enabled, valid CRC field — no error, sets flag") {
    // note-c (from 13f8581): when CRC field is valid and present, no error AND
    // notecardFirmwareSupportsCrc becomes true (auto-detection).
    bool flag = false;
    std::string json = "{\"req\":\"hub.sync\"}";
    std::string response = add_crc(json, 1);
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(flag);  // auto-detected: firmware supports CRC
}

// ---------------------------------------------------------------------------
// crc_check_and_strip — "Supports CRC" section (crc_enabled = true)
// ported from note-c _crcError_test.cpp GIVEN "The Notecard firmware supports CRC"
// ---------------------------------------------------------------------------

TEST_CASE("crc_check_and_strip: CRC enabled, empty string — no error") {
    // note-c: _crcError("", seqNo) == false when notecardFirmwareSupportsCrc
    // (invalid JSON → bail out early before checking CRC expectation)
    bool flag = true;
    std::string response = "";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, invalid JSON — no error") {
    // note-c: _crcError("{\"req\":", seqNo) == false when notecardFirmwareSupportsCrc
    bool flag = true;
    std::string response = "{\"req\":";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, error response — no error") {
    // note-c: err field → skip CRC check even when CRC is expected
    bool flag = true;
    std::string response = R"({"err":"cannot interpret JSON: bool being placed into a non-bool field {io}"})";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, hub.status response without CRC — error") {
    // note-c (from 13f8581): when CRC is enabled, a response long enough to
    // potentially contain a CRC field but lacking one IS an error.
    // This is the same hub.status string used in the regression test above,
    // but now with crc_enabled=true — the absence of CRC is an error.
    bool flag = true;
    std::string response = R"({"connected":false,"status":"connecting"})";
    bool error = check_crc(response, 1, flag);
    CHECK(error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, CRC not at tail — error") {
    // note-c: CRC embedded but not at tail → not detected → error when enabled
    bool flag = true;
    std::string response = R"({"crc":"0009:10BAC79A","req": "hub.sync"})";
    bool error = check_crc(response, 1, flag);
    CHECK(error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, wrong CRC value — error") {
    // note-c: WHEN "CRC doesn't match" → error
    bool flag = true;
    std::string response = R"({"req":"hub.sync","crc":"0001:DEADBEEF"})";
    bool error = check_crc(response, 1, flag);
    CHECK(error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, wrong sequence number — error") {
    // note-c: WHEN "Sequence number doesn't match" → error
    bool flag = true;
    std::string response = R"({"req":"hub.sync","crc":"0009:10BAC79A"})";
    bool error = check_crc(response, 1, flag);  // expected_seq=1, got 9
    CHECK(error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, everything matches — no error, field stripped") {
    // note-c: WHEN "Everything matches" → no error
    bool flag = true;
    std::string json = "{\"req\":\"hub.sync\"}";
    std::string response = add_crc(json, 1);
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(response == json);  // CRC field stripped
}

TEST_CASE("crc_check_and_strip: CRC enabled, trailing CRLF — no error") {
    // note-c: "a trailing CRLF" → trimmed before validation, no error
    bool flag = true;
    std::string json = "{\"req\":\"hub.sync\"}";
    std::string response = add_crc(json, 1) + "\r\n";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
}

TEST_CASE("crc_check_and_strip: CRC enabled, short response without CRC — error") {
    // note-c (from 13f8581): too-short response when CRC is enabled → error
    bool flag = true;
    std::string response = R"({"connected":true})";  // 18 chars < 24 minimum
    bool error = check_crc(response, 1, flag);
    CHECK(error);
}

// ---------------------------------------------------------------------------
// crc_check_and_strip — additional note-cpp specific tests
// ---------------------------------------------------------------------------

TEST_CASE("crc_check_and_strip: CRC not enabled, short response without CRC — no error") {
    // Counterpart to "short response without CRC — error": with flag=false, no error.
    bool flag = false;
    std::string response = R"({"connected":true})";
    bool error = check_crc(response, 1, flag);
    CHECK(!error);
    CHECK(!flag);
    CHECK(response == R"({"connected":true})");  // unchanged
}

TEST_CASE("crc_check_and_strip: corrupted CRC value — error") {
    bool flag = false;
    std::string response = add_crc("{\"ok\":true}", 1);
    // Corrupt the last hex digit of the checksum
    auto crc_pos = response.find("\"crc\":\"");
    REQUIRE(crc_pos != std::string::npos);
    char& c = response[crc_pos + 7 + 5 + 7];  // into the CCCCCCCC part
    c = (c == 'A') ? 'B' : 'A';
    bool error = check_crc(response, 1, flag);
    REQUIRE(error);
}

// ---------------------------------------------------------------------------
// Round-trip: crc_add then crc_check_and_strip restores original
// ---------------------------------------------------------------------------

TEST_CASE("crc round-trip: add then check_and_strip restores original") {
    const std::string original = R"({"req":"note.add","file":"sensors.qo"})";
    uint16_t seq = 42;
    bool flag = false;

    std::string with_crc = add_crc(original, seq);
    REQUIRE(with_crc != original);

    bool error = check_crc(with_crc, seq, flag);
    REQUIRE(!error);
    REQUIRE(flag);
    REQUIRE(with_crc == original);
}

TEST_CASE("crc round-trip works across multiple sequential sequence numbers") {
    bool flag = false;
    for (uint16_t seq = 0; seq < 10; ++seq) {
        const std::string original = R"({"req":"hub.set"})";
        std::string with_crc = add_crc(original, seq);
        bool error = check_crc(with_crc, seq, flag);
        REQUIRE(!error);
        REQUIRE(with_crc == original);
    }
}

TEST_CASE("crc_check_and_strip: lowercase hex digits in CRC field — no error") {
    // read_hex() supports a-f in addition to A-F.  crc_add always emits uppercase,
    // so we manufacture a lowercase CRC by lowercasing the value portion after
    // crc_add, then verify crc_check_and_strip still accepts it.
    bool flag = true;
    const std::string original = R"({"req":"hub.set"})";
    std::string response = add_crc(original, 3);
    // Lowercase every hex letter in the SSSS:CCCCCCCC value (positions after "crc":"").
    auto pos = response.find("\"crc\":\"");
    REQUIRE(pos != std::string::npos);
    for (size_t i = pos + 7; i < response.size() - 1; ++i) {
        char& c = response[i];
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    bool error = check_crc(response, 3, flag);
    CHECK(!error);
    CHECK(response == original);  // CRC field stripped
}

// ---------------------------------------------------------------------------
// CrcAccumulator — delayed ring buffer for streaming CRC
// ---------------------------------------------------------------------------

TEST_CASE("CrcAccumulator: no-CRC response accumulates all bytes") {
    const char* json = R"({"status":"ok"})";
    size_t len = strlen(json);

    CrcAccumulator acc;
    acc.feed(json, len);

    uint32_t expected = crc32(json, len);
    REQUIRE(acc.finalize_all() == expected);
}

TEST_CASE("CrcAccumulator: CRC response matches crc_add") {
    char buf[256];
    const char* json = R"({"status":"ok"})";
    size_t len = strlen(json);
    memcpy(buf, json, len);

    uint16_t seq = 1;
    size_t wire_len = crc_add(buf, len, sizeof(buf), seq);

    CrcAccumulator acc;
    acc.feed(buf, wire_len);

    uint32_t expected = crc32(json, len);
    REQUIRE(acc.finalize_with_brace() == expected);
}

TEST_CASE("CrcAccumulator: filters frame terminators") {
    const char* json = R"({"x":1})";
    size_t len = strlen(json);

    char buf[64];
    memcpy(buf, json, len);
    buf[len] = '\r';
    buf[len + 1] = '\n';

    CrcAccumulator acc;
    acc.feed(buf, len + 2);

    uint32_t expected = crc32(json, len);
    REQUIRE(acc.finalize_all() == expected);
}

TEST_CASE("CrcAccumulator: chunked feed matches single feed") {
    char buf[256];
    const char* json = R"({"temp":22.5,"label":"room"})";
    size_t len = strlen(json);
    memcpy(buf, json, len);
    size_t wire_len = crc_add(buf, len, sizeof(buf), 3);

    CrcAccumulator acc1;
    acc1.feed(buf, wire_len);

    CrcAccumulator acc2;
    for (size_t i = 0; i < wire_len; i += 3) {
        size_t chunk = std::min(size_t(3), wire_len - i);
        acc2.feed(buf + i, chunk);
    }

    REQUIRE(acc1.finalize_with_brace() == acc2.finalize_with_brace());
}

TEST_CASE("CrcAccumulator: short response (shorter than ring)") {
    const char* json = R"({})";
    size_t len = strlen(json);

    CrcAccumulator acc;
    acc.feed(json, len);

    uint32_t expected = crc32(json, len);
    REQUIRE(acc.finalize_all() == expected);
}
