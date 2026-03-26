/// @file test_serial.cpp
/// Integration tests exercising the Notecard over serial UART.
///
/// Compiled only when RX1 and TX1 are defined (i.e. when building
/// with the `serial` or `both` PlatformIO environment).

#include "../include/hal_serial.hpp"
#ifdef NOTECARD_TEST_SERIAL

#include <doctest.h>
#include <note/notecard.hpp>
#include <note/error.hpp>
#include <note/api.hpp>
#include <note/body.hpp>
#include <note/backends/cjson.hpp>
#include <note/transport/serial.hpp>
#include <note/transport/cobs.hpp>
#include "../include/cobs.hpp"  // cobs_encoded_size (legacy, used for verification)
#include "../include/md5.hpp"

namespace {

using SerialTransport = note::transport::NotecardSerial<>;
using Api = note::Api<>;

struct Fixture {
    SerialHal hal{notecardUart()};
    SerialTransport transport{hal};
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

TEST_SUITE("serial") {

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
        .resetTracker()
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

// ─── ATTN payload ───────────────────────────────────────────────────────────

TEST_CASE("card.attn payload without sleep") {
    Fixture f;
    auto& nc = f.nc;

    // Store a payload via card.attn without entering sleep mode.
    // Question: does the payload persist without sleep?
    auto arm = nc.card.attn().arm();
    arm.payload("test-payload-no-sleep");
    auto arm_rsp = arm.execute();
    if (!arm_rsp) { INFO(note::to_string(arm_rsp.error())); }
    REQUIRE(arm_rsp);

    // Retrieve with start:true — does the payload come back?
    auto retrieve = nc.card.attn().retrieve().execute();
    if (!retrieve) { INFO(note::to_string(retrieve.error())); }
    REQUIRE(retrieve);

    MESSAGE("time: ", retrieve.time.value());
    MESSAGE("payload: ", retrieve.payload.data());

    // If payload works without sleep, this should match
    CHECK(note::string_view(retrieve.payload) == "test-payload-no-sleep");

    // Clean up
    nc.card.attn().disarm().execute();
}

TEST_CASE("card.attn payload with sleep timer") {
    Fixture f;
    auto& nc = f.nc;

    // Store payload via sleep with a very short timer (1 second)
    auto sleep_req = nc.card.attn().sleep();
    sleep_req.seconds(note::Seconds{1});
    sleep_req.payload("test-payload-with-sleep");
    auto sleep_rsp = sleep_req.execute();
    if (!sleep_rsp) { INFO(note::to_string(sleep_rsp.error())); }
    REQUIRE(sleep_rsp);

    // Wait for the sleep timer to expire
    f.hal.delay(2000);

    // Retrieve — should get the payload back with a non-zero time
    auto retrieve = nc.card.attn().retrieve().execute();
    if (!retrieve) { INFO(note::to_string(retrieve.error())); }
    REQUIRE(retrieve);

    MESSAGE("time: ", retrieve.time.value());
    MESSAGE("payload: ", retrieve.payload.data());

    CHECK(retrieve.time.value() != 0);
    CHECK(note::string_view(retrieve.payload) == "test-payload-with-sleep");

    // Clean up
    nc.card.attn().disarm().execute();
}

// ─── Error handling ─────────────────────────────────────────────────────────

TEST_CASE("note.add max limit produces Notecard error") {
    Fixture f;
    auto& nc = f.nc;
    const char* file = "integration-err.qo";

    // Clean up from any prior run
    nc.file.remove(file).execute();

    // Add first note — should succeed
    auto r1 = nc.note.add().file(file).body(R"({"a":1})").max(1).execute();
    if (!r1) { INFO(note::to_string(r1.error())); }
    REQUIRE(r1);

    // Add second note — should fail with a Notecard error
    auto r2 = nc.note.add().file(file).body(R"({"a":2})").max(1).execute();
    REQUIRE_FALSE(r2);
    REQUIRE(r2.error().code == note::Error::Notecard);

    // Log the real error string from the Notecard
    MESSAGE("Notecard error: ", r2.error().message);

    // Clean up
    nc.file.remove(file).execute();
}

// ─── Binary data transfer ───────────────────────────────────────────────────
//
// The card.binary protocol differs from normal JSON request/response:
//   1. JSON handshake (card.binary.put or card.binary.get)
//   2. Raw COBS-encoded bytes sent/received directly on the wire
//
// For serial, raw bytes are streamed with a newline terminator.
// These tests exercise the full binary lifecycle with various payload types.

namespace {

/// Put binary data to the Notecard over serial and verify the round-trip.
/// Handles: clear → put JSON → raw transmit → verify → get JSON → raw receive → decode → verify
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

    // Compute MD5 of the unencoded data.
    size_t cobs_max = note::cobs_encoded_size(data_len);
    std::string md5 = md5_hex(data, data_len);

    // ── PUT phase ──────────────────────────────────────────────────

    // COBS encode into a buffer to get the actual encoded length.
    std::vector<uint8_t> encoded(cobs_max + 1);
    size_t actual_cobs_len = 0;
    note::CobsEncoder encoder;
    encoder.encode(data, data_len, [&](const uint8_t* block, size_t n) {
        memcpy(encoded.data() + actual_cobs_len, block, n);
        actual_cobs_len += n;
    });
    encoded[actual_cobs_len] = '\n';
    MESSAGE(label, ": actual_cobs_len=", actual_cobs_len, " md5=", md5.c_str());

    // JSON handshake: tell the Notecard the actual COBS byte count.
    auto put_rsp = nc.card.binary.put()
        .cobs(static_cast<int32_t>(actual_cobs_len))
        .status(md5)
        .execute();
    if (!put_rsp) { INFO(note::to_string(put_rsp.error())); }
    REQUIRE(put_rsp);

    // Transmit the encoded data + EOP.
    bool tx_ok = hal.transmit(encoded.data(), actual_cobs_len + 1);
    REQUIRE(tx_ok);

    // Delay for Notecard to process the binary data
    hal.delay(250);

    // ── Verify stored data ─────────────────────────────────────────

    auto verify_rsp = nc.binary.status().execute();
    if (!verify_rsp) { INFO(note::to_string(verify_rsp.error())); }
    REQUIRE(verify_rsp);
    CHECK(verify_rsp.length == static_cast<int32_t>(data_len));
    CHECK(verify_rsp.cobs == static_cast<int32_t>(actual_cobs_len));
    // Verify the Notecard computed the same MD5 for the stored data
    if (note::string_view(verify_rsp.status).size() > 0) {
        CHECK(note::string_view(verify_rsp.status) == note::string_view(md5));
    }

    // ── GET phase ──────────────────────────────────────────────────

    // JSON handshake: request the binary data back
    auto get_rsp = nc.card.binary.get()
        .cobs(verify_rsp.cobs)
        .length(verify_rsp.length)
        .execute();
    if (!get_rsp) { INFO(note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);

    // Verify MD5 in get response
    if (note::string_view(get_rsp.status).size() > 0) {
        CHECK(note::string_view(get_rsp.status) == note::string_view(md5));
    }

    // Read raw COBS bytes + newline from the wire
    std::vector<uint8_t> rx_buf(actual_cobs_len + 16);
    size_t total_rx = 0;
    uint32_t deadline = hal.millis() + 5000;
    while (total_rx < (actual_cobs_len + 1) && hal.millis() < deadline) {
        size_t n = hal.receive(rx_buf.data() + total_rx, rx_buf.size() - total_rx);
        total_rx += n;
        if (n == 0) hal.delay(10);
    }
    REQUIRE(total_rx >= actual_cobs_len);

    // ── Streaming decode and verify ────────────────────────────────

    // Strip trailing newline if present
    size_t decode_len = total_rx;
    if (decode_len > 0 && rx_buf[decode_len - 1] == '\n') {
        decode_len--;
    }

    // Streaming COBS decode — feed received bytes, collect decoded output.
    note::CobsDecoder decoder;
    std::vector<uint8_t> decoded;
    decoded.reserve(data_len);
    auto sink = [&](const uint8_t* d, size_t n) {
        decoded.insert(decoded.end(), d, d + n);
    };
    decoder.feed(rx_buf.data(), decode_len, sink);
    decoder.flush(sink);

    REQUIRE(decoded.size() == data_len);
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

TEST_CASE("card.binary put + get — 512-byte payload") {
    // Larger payload to exercise streaming on the serial transport.
    Fixture f;
    uint8_t data[512];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    binary_round_trip(f, data, sizeof(data), "512B");
}

// ─── Error handling ─────────────────────────────────────────────────────────

TEST_CASE("bad request returns Notecard error") {
    Fixture f;
    auto& nc = f.nc;
    auto rsp = nc.note.pop("nonexistent-file.qi").execute();
    CHECK(!rsp);
    if (!rsp) {
        CHECK(rsp.error().code == note::Error::Notecard);
        MESSAGE("error: ", note::to_string(rsp.error()));
    }
}

} // TEST_SUITE

#endif // NOTECARD_TEST_SERIAL
