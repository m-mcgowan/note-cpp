// Tests for binary transfer pipeline: do_binary_send, do_binary_receive,
// COBS encode/decode via transport write/read, MD5 verification.

#include <doctest.h>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/notecard.hpp>
#include <note/md5.hpp>
#include <note/transport/cobs.hpp>
#include <note/backends/buffer.hpp>
#include <note/api/card_binary_put.hpp>
#include <note/api/card_binary_get.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Mock transport that records write() calls and replays read() data.
// ---------------------------------------------------------------------------
struct BinaryTestHarness {
    note::test::TestJsonBackend backend;
    std::string last_request;

    // Binary write capture
    std::vector<uint8_t> written_bytes;

    // Binary read replay
    std::vector<uint8_t> read_data;
    size_t read_offset = 0;

    // JSON response to return for the handshake
    std::string json_response = "{}";

    note::test::CallbackTransport transport;
    note::Notecard nc;

    BinaryTestHarness()
        : transport(
            [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                last_request = std::string(req);
                return note::string_view(json_response);
            },
            [this](note::string_view req) -> note::Result<void> {
                last_request = std::string(req);
                return {};
            })
        , nc(note::test::make_test_notecard(backend, transport))
    {
        transport.set_write([this](const uint8_t* data, size_t len) -> note::Result<void> {
            written_bytes.insert(written_bytes.end(), data, data + len);
            return {};
        });
        transport.set_read([this](uint8_t* buf, size_t max_len, uint32_t) -> note::Result<size_t> {
            if (read_offset >= read_data.size())
                return note::make_error(note::Error::ResponseLost, "no more data");
            size_t avail = read_data.size() - read_offset;
            size_t n = std::min(avail, max_len);
            memcpy(buf, read_data.data() + read_offset, n);
            read_offset += n;
            return n;
        });
    }

    // Helper: set up read_data with COBS-encoded version of raw + EOP
    void prepare_cobs_read(const uint8_t* raw, size_t len) {
        read_data.clear();
        read_offset = 0;
        note::CobsEncoder encoder;
        encoder.encode(raw, len, [this](const uint8_t* block, size_t n) {
            read_data.insert(read_data.end(), block, block + n);
        });
        read_data.push_back(note::cobs_eop);
    }

    // Helper: set JSON response with a status (MD5) field
    void set_get_response(note::string_view md5) {
        json_response = std::string("{\"status\":\"") + std::string(md5) + "\"}";
    }
};

// Harness with real JSON parsing — needed for binary GET tests that verify
// response fields (status/MD5). Uses BufferJsonBackend with jsmn.
struct BinaryGetHarness {
    note::backends::BufferJsonBackend<512, 32> backend;
    std::string last_request;
    std::vector<uint8_t> read_data;
    size_t read_offset = 0;
    std::string json_response = "{}";

    note::test::CallbackTransport transport;
    note::Notecard nc;

    BinaryGetHarness()
        : transport(
            [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                last_request = std::string(req);
                return note::string_view(json_response);
            })
        , nc(note::test::make_test_notecard(backend, transport))
    {
        transport.set_write([](const uint8_t*, size_t) -> note::Result<void> { return {}; });
        transport.set_read([this](uint8_t* buf, size_t max_len, uint32_t) -> note::Result<size_t> {
            if (read_offset >= read_data.size())
                return note::make_error(note::Error::ResponseLost, "no more data");
            size_t n = std::min(read_data.size() - read_offset, max_len);
            memcpy(buf, read_data.data() + read_offset, n);
            read_offset += n;
            return n;
        });
    }

    void prepare_cobs_read(const uint8_t* raw, size_t len) {
        read_data.clear();
        read_offset = 0;
        note::CobsEncoder encoder;
        encoder.encode(raw, len, [this](const uint8_t* block, size_t n) {
            read_data.insert(read_data.end(), block, block + n);
        });
        read_data.push_back(note::cobs_eop);
    }

    void set_get_response(note::string_view md5) {
        json_response = std::string("{\"status\":\"") + std::string(md5) + "\"}";
    }
};

} // namespace

// ---------------------------------------------------------------------------
// SoftwareMd5
// ---------------------------------------------------------------------------

TEST_CASE("SoftwareMd5 computes correct hex digest") {
    note::SoftwareMd5 md5;

    // Empty input
    auto empty = md5.compute(nullptr, 0);
    REQUIRE(empty == "d41d8cd98f00b204e9800998ecf8427e");

    // "abc"
    const uint8_t abc[] = {'a', 'b', 'c'};
    auto abc_hash = md5.compute(abc, 3);
    REQUIRE(abc_hash == "900150983cd24fb0d6963f7d28e17f72");

    // 512 bytes of pattern data
    uint8_t data[512];
    for (size_t i = 0; i < 512; i++) data[i] = static_cast<uint8_t>(i & 0xFF);
    auto hash = md5.compute(data, 512);
    REQUIRE(hash.size() == 32);
    // Verify it's a valid hex string
    for (size_t i = 0; i < hash.size(); ++i) {
        char c = hash.buf[i];
        REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}

// ---------------------------------------------------------------------------
// cobs_encoded_length
// ---------------------------------------------------------------------------

TEST_CASE("cobs_encoded_length matches actual encoder output") {
    note::CobsEncoder encoder;

    SUBCASE("no zeros — matches max estimate") {
        uint8_t data[100];
        for (size_t i = 0; i < 100; i++) data[i] = static_cast<uint8_t>(i + 1);
        size_t predicted = note::cobs_encoded_length(data, 100);
        size_t actual = 0;
        encoder.encode(data, 100, [&](const uint8_t*, size_t n) { actual += n; });
        REQUIRE(predicted == actual);
        REQUIRE(predicted == note::cobs_encoded_size(100));
    }

    SUBCASE("all zeros — exact matches max (zeros become code bytes)") {
        uint8_t data[100];
        memset(data, 0, 100);
        size_t predicted = note::cobs_encoded_length(data, 100);
        size_t actual = 0;
        encoder.encode(data, 100, [&](const uint8_t*, size_t n) { actual += n; });
        REQUIRE(predicted == actual);
        REQUIRE(predicted == note::cobs_encoded_size(100));
    }

    SUBCASE("254-byte block boundary") {
        uint8_t data[254];
        for (size_t i = 0; i < 254; i++) data[i] = static_cast<uint8_t>(i + 1);
        size_t predicted = note::cobs_encoded_length(data, 254);
        size_t actual = 0;
        encoder.encode(data, 254, [&](const uint8_t*, size_t n) { actual += n; });
        REQUIRE(predicted == actual);
    }

    SUBCASE("512 bytes with scattered zeros") {
        uint8_t data[512];
        for (size_t i = 0; i < 512; i++) data[i] = static_cast<uint8_t>((i * 7) & 0xFF);
        size_t predicted = note::cobs_encoded_length(data, 512);
        size_t actual = 0;
        encoder.encode(data, 512, [&](const uint8_t*, size_t n) { actual += n; });
        REQUIRE(predicted == actual);
    }

    SUBCASE("empty") {
        REQUIRE(note::cobs_encoded_length(nullptr, 0) == 1);
    }
}

// ---------------------------------------------------------------------------
// Binary PUT pipeline (do_binary_send)
// ---------------------------------------------------------------------------

TEST_CASE("Binary PUT: COBS encodes and writes via transport") {
    BinaryTestHarness h;

    uint8_t data[] = {1, 2, 3, 4, 5, 0, 6, 7, 8};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify(false);  // skip verify for basic test

    auto rsp = h.nc.execute(req);  // const execute — copy-and-execute path
    REQUIRE(rsp);

    // Verify JSON handshake included cobs and status fields
    REQUIRE(h.last_request.find("\"cobs\":") != std::string::npos);
    REQUIRE(h.last_request.find("\"status\":") != std::string::npos);

    // Verify cobs field matches exact encoded length
    auto expected_cobs = note::cobs_encoded_length(data, sizeof(data));
    auto cobs_pos = h.last_request.find("\"cobs\":");
    REQUIRE(cobs_pos != std::string::npos);
    auto cobs_val = std::stoi(h.last_request.substr(cobs_pos + 7));
    REQUIRE(static_cast<size_t>(cobs_val) == expected_cobs);

    // Verify written bytes end with EOP
    REQUIRE(!h.written_bytes.empty());
    REQUIRE(h.written_bytes.back() == note::cobs_eop);

    // Verify COBS data length matches
    REQUIRE(h.written_bytes.size() == expected_cobs + 1);  // +1 for EOP

    // Verify round-trip: decode the written COBS data and compare
    std::vector<uint8_t> decoded;
    note::CobsDecoder decoder;
    auto cobs_data = h.written_bytes.data();
    auto cobs_len = h.written_bytes.size() - 1;  // exclude EOP
    decoder.feed(cobs_data, cobs_len, [&](const uint8_t* d, size_t n) {
        decoded.insert(decoded.end(), d, d + n);
    });
    decoder.flush([&](const uint8_t* d, size_t n) {
        decoded.insert(decoded.end(), d, d + n);
    });
    REQUIRE(decoded.size() == sizeof(data));
    REQUIRE(memcmp(decoded.data(), data, sizeof(data)) == 0);
}

TEST_CASE("Binary PUT: MD5 status matches data") {
    BinaryTestHarness h;

    uint8_t data[] = {'h', 'e', 'l', 'l', 'o'};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify(false);
    h.nc.execute(req);

    note::SoftwareMd5 md5;
    auto expected_md5 = md5.compute(data, sizeof(data));
    REQUIRE(h.last_request.find(expected_md5) != std::string::npos);
}

// ---------------------------------------------------------------------------
// Binary GET pipeline (do_binary_receive)
// ---------------------------------------------------------------------------

TEST_CASE("Binary GET: reads and COBS-decodes from transport") {
    BinaryGetHarness h;

    uint8_t original[] = {10, 20, 30, 0, 40, 50};
    note::SoftwareMd5 md5;
    auto expected_md5 = md5.compute(original, sizeof(original));

    h.prepare_cobs_read(original, sizeof(original));
    h.set_get_response(expected_md5);

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));
    req.length = static_cast<int32_t>(sizeof(original));

    auto rsp = h.nc.execute(req);
    REQUIRE(rsp);
    REQUIRE(memcmp(dst, original, sizeof(original)) == 0);
}

TEST_CASE("Binary GET: MD5 mismatch returns error") {
    BinaryGetHarness h;

    uint8_t original[] = {1, 2, 3, 4, 5};

    h.prepare_cobs_read(original, sizeof(original));
    h.set_get_response("00000000000000000000000000000000");

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));
    req.length = static_cast<int32_t>(sizeof(original));

    auto rsp = h.nc.execute(req);
    REQUIRE(!rsp);
    REQUIRE(rsp.error().code == note::Error::ResponseLost);
}

TEST_CASE("Binary GET: empty MD5 in response skips verification") {
    BinaryGetHarness h;

    uint8_t original[] = {1, 2, 3};
    h.prepare_cobs_read(original, sizeof(original));
    h.json_response = "{}";

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));
    req.length = static_cast<int32_t>(sizeof(original));

    auto rsp = h.nc.execute(req);
    REQUIRE(rsp);
    REQUIRE(memcmp(dst, original, sizeof(original)) == 0);
}

// ---------------------------------------------------------------------------
// No binary data → falls through to JSON execute
// ---------------------------------------------------------------------------

TEST_CASE("Binary PUT without data() falls through to JSON") {
    BinaryTestHarness h;

    note::api::CardBinaryPut req;
    req.cobs = 100;
    req.status = "abc";
    h.nc.execute(req);

    // Should be a normal JSON request, no binary write
    REQUIRE(h.written_bytes.empty());
    REQUIRE(h.last_request.find("\"cobs\":100") != std::string::npos);
}

TEST_CASE("Binary GET without into() falls through to JSON") {
    BinaryTestHarness h;

    note::api::CardBinaryGet req;
    req.length = 100;
    h.nc.execute(req);

    REQUIRE(h.written_bytes.empty());
    REQUIRE(h.last_request.find("\"length\":100") != std::string::npos);
}

// ---------------------------------------------------------------------------
// CallbackTransport write/read without callbacks
// ---------------------------------------------------------------------------

TEST_CASE("CallbackTransport write/read return errors without callbacks") {
    note::test::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });

    uint8_t buf[4] = {1, 2, 3, 4};
    auto w = transport.write(buf, 4);
    REQUIRE(!w);

    auto r = transport.read(buf, 4, 1000);
    REQUIRE(!r);
}

// ---------------------------------------------------------------------------
// const execute() triggers binary pipeline via copy
// ---------------------------------------------------------------------------

TEST_CASE("Binary PUT: const execute() triggers binary pipeline") {
    BinaryTestHarness h;

    note::api::CardBinaryPut put;
    put.data(reinterpret_cast<const uint8_t*>("hello"), 5).verify(false);

    // This calls the const execute() which detects binary data and copies
    h.nc.execute(put);

    // Binary bytes were written (not just JSON)
    REQUIRE(!h.written_bytes.empty());
    REQUIRE(h.written_bytes.back() == note::cobs_eop);
}

TEST_CASE("Binary GET: const execute() triggers binary pipeline") {
    BinaryGetHarness h;

    uint8_t original[] = {0xAA, 0xBB, 0xCC};
    note::SoftwareMd5 md5;
    h.prepare_cobs_read(original, sizeof(original));
    h.set_get_response(md5.compute(original, sizeof(original)));

    uint8_t dst[64] = {};
    note::api::CardBinaryGet get;
    get.into(dst, sizeof(dst)).length(static_cast<int32_t>(sizeof(original)));

    // const execute() detects buffer and copies
    auto rsp = h.nc.execute(get);
    REQUIRE(rsp);
    REQUIRE(memcmp(dst, original, sizeof(original)) == 0);
}

// ---------------------------------------------------------------------------
// Post-transmit verification (verify flag)
// ---------------------------------------------------------------------------

// Harness with controllable transact responses for verify tests.
struct VerifyTestHarness {
    note::backends::BufferJsonBackend<512, 32> backend;
    std::vector<uint8_t> written_bytes;
    int transact_count = 0;
    std::vector<std::string> responses;  // queued JSON responses

    note::test::CallbackTransport transport;
    note::Notecard nc;

    VerifyTestHarness(std::initializer_list<std::string> resps)
        : responses(resps)
        , transport(
            [this](note::string_view, uint32_t) -> note::Result<note::string_view> {
                size_t idx = static_cast<size_t>(transact_count++);
                if (idx < responses.size())
                    return note::string_view(responses[idx]);
                return note::string_view("{}");
            })
        , nc(note::test::make_test_notecard(backend, transport))
    {
        transport.set_write([this](const uint8_t* d, size_t n) -> note::Result<void> {
            written_bytes.insert(written_bytes.end(), d, d + n);
            return {};
        });
    }
};

TEST_CASE("Binary PUT: verify() does pre-flight and post-transmit checks") {
    uint8_t data[] = {1, 2, 3};
    note::SoftwareMd5 md5;
    auto expected_md5 = md5.compute(data, sizeof(data));

    // verify pipeline: reset → status(pre) → PUT handshake → [COBS] → status(post)
    VerifyTestHarness h({
        "{}",                                                    // reset (card.binary delete)
        "{\"max\":1024}",                                        // pre-flight status
        "{}",                                                    // PUT handshake
        std::string("{\"status\":\"") + expected_md5.data() + "\"}"  // post-verify status
    });

    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify();

    auto rsp = h.nc.execute(req);
    REQUIRE(rsp);
    CHECK(h.transact_count == 4);  // reset + pre-status + handshake + post-status
}

TEST_CASE("Binary PUT: verify detects post-transmit MD5 mismatch") {
    uint8_t data[] = {1, 2, 3};

    VerifyTestHarness h({
        "{}",                                                    // reset
        "{\"max\":1024}",                                        // pre-flight status
        "{}",                                                    // PUT handshake
        "{\"status\":\"00000000000000000000000000000000\"}"       // wrong MD5
    });

    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));  // verify=true by default

    auto rsp = h.nc.execute(req);
    REQUIRE(!rsp);
    CHECK(rsp.error().code == note::Error::ResponseLost);
}

TEST_CASE("Binary PUT: verify detects insufficient space") {
    uint8_t data[100];
    memset(data, 0x42, sizeof(data));

    VerifyTestHarness h({
        "{}",              // reset
        "{\"max\":10}",    // only 10 bytes available — data is 100
    });

    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = h.nc.execute(req);
    REQUIRE(!rsp);
    CHECK(rsp.error().code == note::Error::Overflow);
}

TEST_CASE("Binary PUT: verify(false) skips status check") {
    VerifyTestHarness h({"{}"});

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify(false);

    auto rsp = h.nc.execute(req);
    REQUIRE(rsp);
    CHECK(h.transact_count == 1);  // handshake only
}
