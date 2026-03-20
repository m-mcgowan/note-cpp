/// @file test_i2c.cpp
/// Integration tests exercising the Notecard over I2C.
///
/// Compiled only when NOTECARD_I2C_SDA and NOTECARD_I2C_SCL are defined
/// (i.e. when building with the `i2c` or `both` PlatformIO environment).

#include "../include/hal_i2c.hpp"
#ifdef NOTECARD_TEST_I2C

#include <doctest.h>
#include <note/notecard.hpp>
#include <note/error.hpp>
#include <note/api.hpp>
#include <note/body.hpp>
#include <note/backends/cjson.hpp>
#include <note/transport/i2c.hpp>
#include "../include/cobs.hpp"
#include "../include/md5.hpp"

#include <algorithm>

// I2C bus for Notecard. Wire(0) is the default I2C peripheral.
// Must NOT be a file-scope static: TwoWire's constructor creates FreeRTOS
// primitives, but global constructors run before the scheduler is ready.
static TwoWire& notecardWire() {
    static TwoWire wire(0);
    return wire;
}

namespace {

using I2cTransport = note::transport::NotecardI2c<>;
using Api = note::Api<>;

struct Fixture {
    Esp32I2CHal hal{notecardWire()};
    I2cTransport transport{hal};
    note::backends::CjsonBackend backend;
    note::Notecard notecard{backend, transport};
    Api nc{notecard};
};

struct SensorData {
    float temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

} // namespace

TEST_SUITE("i2c") {

// ─── Basic request/response ─────────────────────────────────────────────────

TEST_CASE("card.version returns valid device info") {
    Fixture f;
    auto& nc = f.nc;
    auto rsp = nc.card.version().execute();
    if (!rsp) { INFO(note::to_string(rsp.error())); }
    REQUIRE(rsp);
    CHECK(!note::string_view(rsp.device).empty());
    CHECK(!note::string_view(rsp.version).empty());
    MESSAGE("device: ", rsp.device);
    MESSAGE("version: ", rsp.version);
}

TEST_CASE("card.status returns operational state") {
    Fixture f;
    auto& nc = f.nc;
    auto rsp = nc.card.status().execute();
    if (!rsp) { INFO(note::to_string(rsp.error())); }
    REQUIRE(rsp);
    CHECK(!note::string_view(rsp.status).empty());
    MESSAGE("status: ", rsp.status);
    MESSAGE("storage: ", rsp.storage, "%");
}

// ─── Configuration round-trip ───────────────────────────────────────────────

TEST_CASE("hub.set + hub.get round-trip") {
    Fixture f;
    auto& nc = f.nc;
    auto set_rsp = nc.hub.set()
        .product("com.example.integration-test")
        .execute();
    if (!set_rsp) { INFO(note::to_string(set_rsp.error())); }
    REQUIRE(set_rsp);

    auto get_rsp = nc.hub.get().execute();
    if (!get_rsp) { INFO(note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);
    CHECK(note::string_view(get_rsp.product) == "com.example.integration-test");
}

// ─── Note lifecycle ─────────────────────────────────────────────────────────

TEST_CASE("note.add sends a note") {
    Fixture f;
    auto& nc = f.nc;
    auto rsp = nc.note.add()
        .file("integration-test.qo")
        .execute();
    if (!rsp) { INFO(note::to_string(rsp.error())); }
    REQUIRE(rsp);
}

TEST_CASE("note.update + note.get body round-trip") {
    Fixture f;
    auto& nc = f.nc;
    const char* file = "integration-body.db";
    const char* noteId = "test-sensor";

    // note.update creates or replaces — idempotent across test runs
    SensorData sent{.temperature = 23.5f, .humidity = 65};
    auto update_rsp = nc.note.update(file, noteId)
        .body(sent)
        .execute();
    if (!update_rsp) { MESSAGE("update error: ", note::to_string(update_rsp.error())); }
    REQUIRE(update_rsp);

    auto get_rsp = nc.note.read(file)
        .noteId(noteId)
        .execute();
    if (!get_rsp) { MESSAGE("get error: ", note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);

    SensorData received = get_rsp.bodyAs<SensorData>();
    CHECK(received.temperature == doctest::Approx(sent.temperature));
    CHECK(received.humidity == sent.humidity);
}

TEST_CASE("note.changes tracks additions") {
    Fixture f;
    auto& nc = f.nc;
    const char* file = "integration-changes.db";
    const char* tracker = "integration-test";

    // Reset tracker
    auto reset_rsp = nc.note.changes().peek()
        .file(file)
        .tracker(tracker)
        .start(true)
        .execute();
    if (!reset_rsp) { MESSAGE("reset error: ", note::to_string(reset_rsp.error())); }

    // Delete any leftover note, then add
    nc.note.remove(file, "test-change").execute();
    auto add_rsp = nc.note.add().file(file).noteId("test-change").execute();
    if (!add_rsp) { MESSAGE("add error: ", note::to_string(add_rsp.error())); }
    REQUIRE(add_rsp);

    // Check for changes
    auto rsp = nc.note.changes().peek()
        .file(file)
        .tracker(tracker)
        .execute();
    if (!rsp) { MESSAGE("changes error: ", note::to_string(rsp.error())); }
    REQUIRE(rsp);
    CHECK(rsp.changes > 0);
    MESSAGE("changes: ", rsp.changes, " total: ", rsp.total);

    // Clean up (.db needs note.delete)
    nc.note.remove(file, "test-change").execute();
}

// ─── Environment variables ──────────────────────────────────────────────────

TEST_CASE("env.default set + get round-trip") {
    Fixture f;
    auto& nc = f.nc;

    auto set_rsp = nc.env.setDefault("_integration_test_var", "hello-from-note-cpp")
        .execute();
    if (!set_rsp) { INFO(note::to_string(set_rsp.error())); }
    REQUIRE(set_rsp);

    // Read it back via env.get
    auto get_rsp = nc.env.get().name("_integration_test_var").execute();
    if (!get_rsp) { INFO(note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);
    CHECK(note::string_view(get_rsp.text) == "hello-from-note-cpp");

    // Clean up
    nc.env.clearDefault("_integration_test_var").execute();
}

// ─── Binary data transfer ───────────────────────────────────────────────────
//
// The card.binary protocol differs from normal JSON request/response:
//   1. JSON handshake (card.binary.put or card.binary.get)
//   2. Raw COBS-encoded bytes sent/received directly on the I2C bus
//
// Unlike serial (which streams all bytes at once), I2C must chunk the raw
// binary data into max_transfer()-sized writes/reads — the same chunking
// the NotecardI2c transport uses for JSON, but without the protocol framing.
//
// These tests exercise the full binary lifecycle including multi-chunk I2C
// transfers with various payload types.

namespace {

/// Chunked I2C binary transmit: send COBS data in max_transfer() chunks.
/// Matches note-c's _i2cChunkedTransmit with delay=false (no pacing for binary).
bool i2c_binary_transmit(Esp32I2CHal& hal, const uint8_t* data, size_t len) {
    const size_t mtu = hal.max_transfer();
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = std::min(len - offset, mtu);
        if (!hal.transmit(data + offset, chunk)) return false;
        offset += chunk;
    }
    return true;
}

/// Chunked I2C binary receive: read COBS data using available-bytes feedback.
/// Matches note-c's _i2cChunkedReceive: polls available count, only requests
/// what the Notecard has ready, exits when newline found and nothing left.
size_t i2c_binary_receive(Esp32I2CHal& hal, uint8_t* buf, size_t buf_size, uint32_t timeout_ms) {
    const size_t mtu = hal.max_transfer();
    size_t received = 0;
    uint32_t avail = 0;
    uint32_t deadline = hal.millis() + timeout_ms;
    bool found_eop = false;

    // First query: request 0 bytes to get initial available count
    hal.receive(buf, 0, avail);

    while (hal.millis() < deadline) {
        // Request min(available, mtu) bytes
        size_t want = avail;
        if (want > mtu) want = mtu;
        if (received + want > buf_size) want = buf_size - received;

        if (want > 0) {
            bool ok = hal.receive(buf + received, want, avail);
            if (!ok) {
                hal.delay(10);
                continue;
            }
            received += want;

            // Check for newline terminator
            if (received > 0 && buf[received - 1] == '\n') {
                found_eop = true;
            }
        }

        // If we have the EOP and no more data available, we're done
        if (found_eop && avail == 0) {
            break;
        }

        // If nothing available, wait and poll again
        if (avail == 0) {
            hal.delay(50);
            hal.receive(buf + received, 0, avail);
        }
    }
    return received;
}

/// Put binary data to the Notecard over I2C and verify the round-trip.
/// Handles: clear → put JSON → chunked transmit → verify → get JSON → chunked receive → decode → verify
void binary_round_trip(Fixture& f, const uint8_t* data, size_t data_len, const char* label) {
    auto& nc = f.nc;
    auto& hal = f.hal;
    INFO("payload: ", label, " (", data_len, " bytes)");

    // Clear any existing binary data
    nc.binary.clear().execute();

    // Check available space
    auto status_rsp = nc.binary.status().execute();
    if (!status_rsp) { INFO(note::to_string(status_rsp.error())); }
    REQUIRE(status_rsp);
    REQUIRE(status_rsp.max > 0);
    REQUIRE(static_cast<int32_t>(data_len) <= status_rsp.max);
    MESSAGE(label, ": binary max=", status_rsp.max, " bytes, payload=", data_len, " bytes");

    // COBS-encode the data
    std::vector<uint8_t> cobs_buf(cobs_encoded_size(data_len));
    size_t cobs_len = cobs_encode(data, data_len, cobs_buf.data());

    // Compute MD5 of unencoded data
    std::string md5 = md5_hex(data, data_len);
    MESSAGE(label, ": cobs_len=", cobs_len, " md5=", md5.c_str());

    // ── PUT phase ──────────────────────────────────────────────────

    // JSON handshake: tell the Notecard how many COBS bytes are coming.
    // The cobs field is the encoded length WITHOUT the EOP — matches note-c.
    auto put_rsp = nc.card.binaryPut()
        .cobs(static_cast<int32_t>(cobs_len))
        .status(md5)
        .execute();
    if (!put_rsp) { INFO(note::to_string(put_rsp.error())); }
    REQUIRE(put_rsp);

    // Send raw COBS-encoded bytes + newline via chunked I2C writes.
    // Each chunk is at most max_transfer() bytes (253 on ESP32).
    cobs_buf.push_back('\n');
    bool tx_ok = i2c_binary_transmit(hal, cobs_buf.data(), cobs_buf.size());
    REQUIRE(tx_ok);

    // Delay for Notecard to process the binary data
    hal.delay(250);

    // ── Verify stored data ─────────────────────────────────────────

    auto verify_rsp = nc.binary.status().execute();
    if (!verify_rsp) { MESSAGE("verify error: ", note::to_string(verify_rsp.error())); }
    REQUIRE(verify_rsp);
    CHECK(verify_rsp.length == static_cast<int32_t>(data_len));
    CHECK(verify_rsp.cobs == static_cast<int32_t>(cobs_len));
    // Verify the Notecard computed the same MD5 for the stored data
    if (note::string_view(verify_rsp.status).size() > 0) {
        CHECK(note::string_view(verify_rsp.status) == note::string_view(md5));
    }

    // ── GET phase ──────────────────────────────────────────────────

    // JSON handshake: request the binary data back
    auto get_rsp = nc.card.binaryGet()
        .cobs(verify_rsp.cobs)
        .length(verify_rsp.length)
        .execute();
    if (!get_rsp) { INFO(note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);

    // Verify MD5 in get response
    if (note::string_view(get_rsp.status).size() > 0) {
        CHECK(note::string_view(get_rsp.status) == note::string_view(md5));
    }

    // Read raw COBS bytes + newline via chunked I2C reads
    std::vector<uint8_t> rx_buf(cobs_len + 16);
    size_t total_rx = i2c_binary_receive(hal, rx_buf.data(), rx_buf.size(), 5000);
    MESSAGE(label, ": received ", total_rx, " bytes (expected ~", cobs_len + 1, ")");
    // Should receive at least cobs_len bytes (possibly +1 for the newline)
    REQUIRE(total_rx >= cobs_len);

    // ── Decode and verify ──────────────────────────────────────────

    // Strip trailing newline if present before decoding
    size_t decode_len = total_rx;
    if (decode_len > 0 && rx_buf[decode_len - 1] == '\n') {
        decode_len--;
    }

    std::vector<uint8_t> decoded(data_len + 1);
    size_t decoded_len = cobs_decode(rx_buf.data(), decode_len, decoded.data());
    REQUIRE(decoded_len == data_len);
    CHECK(memcmp(decoded.data(), data, data_len) == 0);

    // Clean up
    nc.binary.clear().execute();
}

} // namespace

TEST_CASE("card.binary put + get — text payload") {
    Fixture f;
    const uint8_t data[] = "Hello from note-cpp binary test!";
    binary_round_trip(f, data, sizeof(data) - 1, "text");
}

TEST_CASE("card.binary put + get — data with zero bytes") {
    // COBS encoding exists specifically to handle zero bytes in data.
    // This test ensures the encoder/decoder and Notecard handle them correctly.
    Fixture f;
    uint8_t data[64];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = static_cast<uint8_t>(i % 5 == 0 ? 0 : i);  // zeros every 5th byte
    }
    binary_round_trip(f, data, sizeof(data), "zeros");
}

TEST_CASE("card.binary put + get — 512-byte multi-chunk payload") {
    // 512 bytes exceeds I2C max_transfer (253), forcing multiple I2C
    // transactions for both transmit and receive — verifying that the
    // chunked binary protocol works correctly.
    Fixture f;
    uint8_t data[512];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    binary_round_trip(f, data, sizeof(data), "512B-chunked");
}

// ─── Error handling ─────────────────────────────────────────────────────────

TEST_CASE("bad request returns Notecard error") {
    Fixture f;
    auto& nc = f.nc;
    auto rsp = nc.note.get().pop()
        .file("nonexistent-file.qi")
        .execute();
    CHECK(!rsp);
    if (!rsp) {
        CHECK(rsp.error().code == note::Error::Notecard);
        MESSAGE("error: ", note::to_string(rsp.error()));
    }
}

} // TEST_SUITE

#endif // NOTECARD_TEST_I2C
