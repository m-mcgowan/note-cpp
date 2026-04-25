// Tests for streaming COBS encoder/decoder.
#include <doctest.h>
#include <note/transport/cobs.hpp>
#include <vector>
#include <cstring>

// Reference buffered encoder for verification.
#include "../tests/integration/firmware/include/cobs.hpp"

namespace {

// Helper: streaming encode, collect into a vector.
std::vector<uint8_t> stream_encode(const uint8_t* data, size_t len) {
    note::CobsEncoder encoder;
    std::vector<uint8_t> out;
    encoder.encode(data, len, [&](const uint8_t* block, size_t n) {
        out.insert(out.end(), block, block + n);
    });
    return out;
}

// Helper: streaming decode, collect into a vector.
std::vector<uint8_t> stream_decode(const uint8_t* data, size_t len) {
    note::CobsDecoder decoder;
    std::vector<uint8_t> out;
    auto sink = [&](const uint8_t* block, size_t n) {
        out.insert(out.end(), block, block + n);
    };
    decoder.feed(data, len, sink);
    // Feed termination marker
    uint8_t term = note::cobs_eop;  // code=0 after XOR with eop
    decoder.feed(&term, 1, sink);
    return out;
}

// Helper: reference buffered encode.
std::vector<uint8_t> ref_encode(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(cobs_encoded_size(len));
    size_t n = cobs_encode(data, len, out.data());
    out.resize(n);
    return out;
}

// Round-trip: encode with streaming, decode with streaming, verify.
void roundtrip(const uint8_t* data, size_t len) {
    auto encoded = stream_encode(data, len);
    REQUIRE(encoded == ref_encode(data, len));

    auto decoded = stream_decode(encoded.data(), encoded.size());
    REQUIRE(decoded.size() == len);
    if (len > 0) {
        REQUIRE(memcmp(decoded.data(), data, len) == 0);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Encoder tests
// ---------------------------------------------------------------------------

TEST_CASE("COBS: empty input") {
    roundtrip(nullptr, 0);
}

TEST_CASE("COBS: single nonzero byte") {
    uint8_t d[] = {0x42};
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: single zero byte") {
    uint8_t d[] = {0x00};
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: multiple zeros") {
    uint8_t d[10] = {};
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: short nonzero sequence") {
    uint8_t d[] = {1, 2, 3, 4, 5};
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: mixed data with zeros") {
    uint8_t d[] = {0x11, 0x22, 0x00, 0x33};
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: 254 nonzero bytes (one full block)") {
    uint8_t d[254];
    for (int i = 0; i < 254; i++) d[i] = static_cast<uint8_t>(i + 1);
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: 255 nonzero bytes (forces block split)") {
    uint8_t d[255];
    for (int i = 0; i < 255; i++) d[i] = static_cast<uint8_t>(i + 1);
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: 512 bytes with periodic zeros") {
    uint8_t d[512];
    for (int i = 0; i < 512; i++) d[i] = static_cast<uint8_t>(i & 0xFF);
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: 4KB payload") {
    std::vector<uint8_t> d(4096);
    for (size_t i = 0; i < d.size(); i++) d[i] = static_cast<uint8_t>(i % 256);
    roundtrip(d.data(), d.size());
}

TEST_CASE("COBS: bytes matching EOP not treated specially in input") {
    uint8_t d[] = {0x0A, 0x0A, 0x0A};
    roundtrip(d, sizeof(d));
}

TEST_CASE("COBS: encoded size calculation") {
    REQUIRE(note::cobs_encoded_size(0) == 1);
    REQUIRE(note::cobs_encoded_size(1) == 2);
    REQUIRE(note::cobs_encoded_size(254) == 256);
    REQUIRE(note::cobs_encoded_size(255) == 257);
}

// ---------------------------------------------------------------------------
// Decoder: chunked input
// ---------------------------------------------------------------------------

TEST_CASE("COBS: decode byte-by-byte") {
    uint8_t data[] = {0x11, 0x00, 0x22, 0x33, 0x00, 0x44};
    auto encoded = ref_encode(data, sizeof(data));

    note::CobsDecoder decoder;
    std::vector<uint8_t> decoded;
    auto sink = [&](const uint8_t* block, size_t n) {
        decoded.insert(decoded.end(), block, block + n);
    };

    // Feed one byte at a time
    for (size_t i = 0; i < encoded.size(); i++) {
        decoder.feed(&encoded[i], 1, sink);
    }
    uint8_t term = note::cobs_eop;
    decoder.feed(&term, 1, sink);

    REQUIRE(decoded.size() == sizeof(data));
    REQUIRE(memcmp(decoded.data(), data, sizeof(data)) == 0);
}

TEST_CASE("COBS: decode in random-sized chunks") {
    // Large payload to exercise multiple blocks
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<uint8_t>(i % 256);

    auto encoded = ref_encode(data.data(), data.size());

    note::CobsDecoder decoder;
    std::vector<uint8_t> decoded;
    auto sink = [&](const uint8_t* block, size_t n) {
        decoded.insert(decoded.end(), block, block + n);
    };

    // Feed in chunks of varying size (7, 13, 31...)
    size_t offset = 0;
    size_t chunk_sizes[] = {7, 13, 31, 64, 128, 3, 50, 200};
    size_t ci = 0;
    while (offset < encoded.size()) {
        size_t chunk = chunk_sizes[ci % 8];
        if (offset + chunk > encoded.size()) chunk = encoded.size() - offset;
        decoder.feed(encoded.data() + offset, chunk, sink);
        offset += chunk;
        ci++;
    }
    uint8_t term = note::cobs_eop;
    decoder.feed(&term, 1, sink);

    REQUIRE(decoded.size() == data.size());
    REQUIRE(memcmp(decoded.data(), data.data(), data.size()) == 0);
}

TEST_CASE("COBS: feed returns false on termination") {
    uint8_t data[] = {0x42};
    auto encoded = stream_encode(data, sizeof(data));

    // Append termination marker
    encoded.push_back(note::cobs_eop);

    note::CobsDecoder decoder;
    std::vector<uint8_t> decoded;
    bool more = decoder.feed(encoded.data(), encoded.size(),
        [&](const uint8_t* block, size_t n) {
            decoded.insert(decoded.end(), block, block + n);
        });

    REQUIRE_FALSE(more);
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0] == 0x42);
}

// ---------------------------------------------------------------------------
// Encoder: source is const
// ---------------------------------------------------------------------------

TEST_CASE("COBS: encoder does not modify source") {
    uint8_t data[] = {0x11, 0x00, 0x22, 0x33, 0x00, 0x44};
    uint8_t copy[sizeof(data)];
    memcpy(copy, data, sizeof(data));

    stream_encode(data, sizeof(data));

    REQUIRE(memcmp(data, copy, sizeof(data)) == 0);
}
