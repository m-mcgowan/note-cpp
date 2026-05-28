/// @file test_i2c.cpp
/// I2C-specific integration tests — binary transfer via HAL-level chunking.
///
/// Transport-agnostic tests are in test_notecard_api.cpp (shared via symlink).
/// This file contains only tests that require direct I2C HAL access.

#include "../include/hal_i2c.hpp"
#ifdef NOTECARD_TEST_I2C

#include <doctest.h>
#include "../include/notecard_api_fixture.hpp"

#include <note/notecard.hpp>
#include <note/error.hpp>
#include <note/api.hpp>
#include <note/backends/cjson.hpp>
#include <note/link/i2c.hpp>
#include <note/link/cobs.hpp>
#include <note/protocol.hpp>
#include "../include/md5.hpp"

#include <algorithm>

// I2C bus — must not be a file-scope static (FreeRTOS timing).
static TwoWire& notecardWire() {
    static TwoWire wire(0);
    return wire;
}

namespace {

using Api = note::Api<>;

// I2C Fixture with direct HAL access for binary tests. The full stack is
// Hal → framer → Protocol → Notecard, matching test_serial.cpp. I2cFramer
// is a `note::Hal` (byte conduit with framing), so the Notecard's
// ITransact slot needs a `note::Protocol` wrapping the framer.
struct I2cFixture {
    NotecardI2cHal hal{notecardWire(), NOTECARD_I2C_SDA, NOTECARD_I2C_SCL};
    note::link::I2cFramer<> framer{hal};
    note::Protocol transport{framer};
    note::backends::CjsonBackend backend;
    note::Notecard notecard{backend, transport};
    Api nc{notecard};
};

/// Chunked I2C binary transmit.
bool i2c_binary_transmit(NotecardI2cHal& hal, const uint8_t* data, size_t len) {
    const size_t mtu = hal.max_transfer();
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = std::min(len - offset, mtu);
        if (!hal.transmit(data + offset, chunk)) return false;
        offset += chunk;
    }
    return true;
}

/// Chunked I2C binary receive with available-bytes polling.
size_t i2c_binary_receive(NotecardI2cHal& hal, uint8_t* buf, size_t buf_size, uint32_t timeout_ms) {
    const size_t mtu = hal.max_transfer();
    size_t received = 0;
    uint32_t avail = 0;
    uint32_t deadline = hal.millis() + timeout_ms;
    bool found_eop = false;
    hal.receive(buf, 0, avail);
    while (hal.millis() < deadline) {
        size_t want = avail;
        if (want > mtu) want = mtu;
        if (received + want > buf_size) want = buf_size - received;
        if (want > 0) {
            bool ok = hal.receive(buf + received, want, avail);
            if (!ok) { hal.delay(10); continue; }
            received += want;
            if (received > 0 && buf[received - 1] == '\n') found_eop = true;
        }
        if (found_eop && avail == 0) break;
        if (avail == 0) { hal.delay(50); hal.receive(buf + received, 0, avail); }
    }
    return received;
}

/// HAL-level binary round-trip (bypasses library pipeline).
void binary_round_trip_hal(I2cFixture& f, const uint8_t* data, size_t data_len, const char* label) {
    auto& nc = f.nc;
    auto& hal = f.hal;
    INFO("payload: ", label, " (", data_len, " bytes)");

    nc.card.binary.clear().execute();
    auto status_rsp = nc.card.binary.status().execute();
    REQUIRE(status_rsp);
    REQUIRE(status_rsp.max > 0);
    REQUIRE(static_cast<int32_t>(data_len) <= status_rsp.max);

    size_t cobs_max = note::cobs_encoded_size(data_len);
    auto md5 = md5_hex(data, data_len);

    std::vector<uint8_t> encoded(cobs_max + 1);
    size_t actual_cobs_len = 0;
    note::CobsEncoder encoder;
    encoder.encode(data, data_len, [&](const uint8_t* block, size_t n) {
        memcpy(encoded.data() + actual_cobs_len, block, n);
        actual_cobs_len += n;
    });
    encoded[actual_cobs_len] = '\n';

    auto put_rsp = nc.card.binary.put()
        .cobs(static_cast<int32_t>(actual_cobs_len))
        .status(md5)
        .execute();
    REQUIRE(put_rsp);

    bool tx_ok = i2c_binary_transmit(hal, encoded.data(), actual_cobs_len + 1);
    REQUIRE(tx_ok);
    hal.delay(250);

    auto verify_rsp = nc.card.binary.status().execute();
    REQUIRE(verify_rsp);
    CHECK(verify_rsp.length == static_cast<int32_t>(data_len));
    CHECK(verify_rsp.cobs == static_cast<int32_t>(actual_cobs_len));

    auto get_rsp = nc.card.binary.get()
        .cobs(verify_rsp.cobs)
        .length(verify_rsp.length)
        .execute();
    REQUIRE(get_rsp);

    std::vector<uint8_t> rx_buf(actual_cobs_len + 16);
    size_t total_rx = i2c_binary_receive(hal, rx_buf.data(), rx_buf.size(), 5000);
    REQUIRE(total_rx >= actual_cobs_len);

    size_t decode_len = total_rx;
    if (decode_len > 0 && rx_buf[decode_len - 1] == '\n') decode_len--;

    note::CobsDecoder decoder;
    std::vector<uint8_t> decoded;
    decoded.reserve(data_len);
    auto sink = [&](const uint8_t* d, size_t n) { decoded.insert(decoded.end(), d, d + n); };
    decoder.feed(rx_buf.data(), decode_len, sink);
    decoder.flush(sink);

    REQUIRE(decoded.size() == data_len);
    CHECK(memcmp(decoded.data(), data, data_len) == 0);
    nc.card.binary.clear().execute();
}

} // namespace

TEST_SUITE("i2c") {

TEST_CASE("card.binary put + get — text payload (HAL)") {
    I2cFixture f;
    const uint8_t data[] = "Hello from note-cpp binary test!";
    binary_round_trip_hal(f, data, sizeof(data) - 1, "text");
}

TEST_CASE("card.binary put + get — data with zero bytes (HAL)") {
    I2cFixture f;
    uint8_t data[64];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = static_cast<uint8_t>(i % 5 == 0 ? 0 : i);
    binary_round_trip_hal(f, data, sizeof(data), "zeros");
}

TEST_CASE("card.binary put + get — 512-byte multi-chunk payload (HAL)") {
    I2cFixture f;
    uint8_t data[512];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    binary_round_trip_hal(f, data, sizeof(data), "512B-chunked");
}

} // TEST_SUITE("i2c")

#endif // NOTECARD_TEST_I2C
