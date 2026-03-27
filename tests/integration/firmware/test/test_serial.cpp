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
#include <note/json_sax_streaming.hpp>
#include <note/units.hpp>
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

    // Exploratory: does the Notecard accept a payload without entering sleep?
    // Note: payload must be base64-encoded per the Notecard API.
    auto req = nc.card.attn().request();
    req.payload = "dGVzdC1wYXlsb2FkLW5vLXNsZWVw";  // base64("test-payload-no-sleep")
    auto rsp = req.execute();
    if (!rsp) { MESSAGE("attn error: ", note::to_string(rsp.error())); }
    REQUIRE(rsp);

    // Retrieve — does the payload come back?
    auto retrieve = nc.card.attn().retrieve().execute();
    if (!retrieve) { INFO(note::to_string(retrieve.error())); }
    REQUIRE(retrieve);

    MESSAGE("payload: ", retrieve.payload.data());
    CHECK(note::string_view(retrieve.payload) == "dGVzdC1wYXlsb2FkLW5vLXNsZWVw");

    // Clean up
    nc.card.attn().disarm().execute();
}

TEST_CASE("card.attn payload with sleep timer") {
    Fixture f;
    auto& nc = f.nc;

    // Store payload via sleep with a very short timer (1 second)
    auto sleep_req = nc.card.attn().sleep();
    sleep_req.seconds(note::Seconds{1});
    sleep_req.payload("dGVzdC1wYXlsb2FkLXdpdGgtc2xlZXA=");  // base64("test-payload-with-sleep")
    auto sleep_rsp = sleep_req.execute();
    if (!sleep_rsp) { MESSAGE("sleep error: ", note::to_string(sleep_rsp.error())); }
    REQUIRE(sleep_rsp);

    // Wait for the sleep timer to expire
    f.hal.delay(2000);

    // Retrieve — should get the payload back with a non-zero time
    auto retrieve = nc.card.attn().retrieve().execute();
    if (!retrieve) { MESSAGE("retrieve error: ", note::to_string(retrieve.error())); }
    REQUIRE(retrieve);

    MESSAGE("time: ", retrieve.time.value());
    MESSAGE("payload: ", retrieve.payload.data());

    CHECK(retrieve.time.value() != 0);
    CHECK(note::string_view(retrieve.payload) == "dGVzdC1wYXlsb2FkLXdpdGgtc2xlZXA=");

    // Clean up
    nc.card.attn().disarm().execute();
}

// ─── Error handling ─────────────────────────────────────────────────────────

struct MaxTestPayload { int32_t a; NOTE_FIELDS(a) };

TEST_CASE("note.add max limit produces Notecard error") {
    Fixture f;
    auto& nc = f.nc;
    const char* file = "integration-err.qo";

    // Clean up from any prior run
    nc.file.remove(file).execute();

    // Add first note — should succeed
    auto r1 = nc.note.add().file(file).body(MaxTestPayload{1}).max(1).execute();
    if (!r1) { MESSAGE("add error: ", note::to_string(r1.error())); }
    REQUIRE(r1);

    // Add second note — should fail with a Notecard error
    auto r2 = nc.note.add().file(file).body(MaxTestPayload{2}).max(1).execute();
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
/// Uses the library's binary pipeline: .data()/.into() + execute() handles
/// COBS encode/decode, MD5, and streaming automatically.
void binary_round_trip(Fixture& f, const uint8_t* data, size_t data_len, const char* label) {
    auto& nc = f.nc;
    INFO("payload: ", label, " (", data_len, " bytes)");

    // ── PUT phase — library handles reset, COBS, MD5, verify ──────

    auto put_rsp = nc.card.binary.put()
        .data(data, data_len)
        .execute();  // verify=true: resets, checks space, streams, verifies MD5
    if (!put_rsp) { MESSAGE("put error: ", note::to_string(put_rsp.error())); }
    REQUIRE(put_rsp);

    // ── GET phase — library handles COBS decode + MD5 verify ──────

    // Query status to get length for the GET request
    auto status_rsp = nc.binary.status().execute();
    REQUIRE(status_rsp);
    REQUIRE(status_rsp.length > 0);

    std::vector<uint8_t> dst(data_len);
    auto get_rsp = nc.card.binary.get()
        .into(dst.data(), dst.size())
        .length(status_rsp.length)
        .execute();  // streams, decodes, verifies MD5
    if (!get_rsp) { MESSAGE("get error: ", note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);

    // ── Verify decoded data matches original ──────────────────────

    CHECK(memcmp(dst.data(), data, data_len) == 0);
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

// ─── Streaming SAX parser over real UART ────────────────────────────────────
//
// These tests parse Notecard responses incrementally via sax_parse_streaming,
// reading directly from the serial transport. No full-response buffer.

namespace {

/// Sink that captures specific fields from a card.version response.
struct VersionSink : public note::JsonSink {
    std::string device, version;
    void on_string(note::string_view key, note::string_view val) override {
        if (key == "device")  device.assign(val.data(), val.size());
        if (key == "version") version.assign(val.data(), val.size());
    }
};

/// Sink that captures status from a card.status response.
struct StatusSink : public note::JsonSink {
    std::string status;
    int32_t storage = -1;
    void on_string(note::string_view key, note::string_view val) override {
        if (key == "status") status.assign(val.data(), val.size());
    }
    void on_number(note::string_view key, note::string_view raw) override {
        if (key == "storage") storage = note::parse_int(raw);
    }
};

/// Sink that captures the "err" field (error responses).
struct ErrorSink : public note::JsonSink {
    std::string err;
    void on_string(note::string_view key, note::string_view val) override {
        if (key == "err") err.assign(val.data(), val.size());
    }
};

/// Send a raw JSON request and parse the response via streaming SAX.
/// Uses transport.send() to transmit, transport.read() to receive byte-by-byte.
template<typename Sink>
note::string_view streaming_request(SerialTransport& transport, const char* req_json,
                                     Sink& sink, uint32_t timeout_ms = 10000) {
    auto send_result = transport.send(note::string_view(req_json));
    if (!send_result) return "send failed";

    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> note::Result<size_t> {
        return transport.read(buf, max, timeout);
    };
    return note::sax_parse_streaming(read_fn, timeout_ms, sink);
}

template<typename Sink>
note::string_view streaming_request(SerialTransport& transport, const char* req_json,
                                     note::SaxStreamBuf& sbuf, Sink& sink,
                                     uint32_t timeout_ms = 10000) {
    auto send_result = transport.send(note::string_view(req_json));
    if (!send_result) return "send failed";

    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t timeout) -> note::Result<size_t> {
        return transport.read(buf, max, timeout);
    };
    return note::sax_parse_streaming(read_fn, timeout_ms, sbuf, sink);
}

} // namespace

TEST_CASE("streaming sax: card.version matches execute()") {
    Fixture f;

    // Normal path — copy device (stable hardware ID) before buffer reuse
    auto rsp = f.nc.card.version().execute();
    REQUIRE(rsp);
    std::string expected_device(rsp.device.data(), rsp.device.size());

    // Streaming SAX path
    VersionSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"card.version\"}\n", sink);
    INFO("parse error: ", err.data());
    REQUIRE(err.empty());

    // device is a hardware serial number — must match exactly
    CHECK(sink.device == expected_device);
    // version has known prefix and varies in build metadata between calls
    CHECK(sink.version.substr(0, 9) == "notecard-");
    MESSAGE("streaming device: ", sink.device.c_str());
    MESSAGE("streaming version: ", sink.version.c_str());
}

TEST_CASE("streaming sax: card.status") {
    Fixture f;
    StatusSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"card.status\"}\n", sink);
    INFO("parse error: ", err.data());
    REQUIRE(err.empty());
    CHECK(!sink.status.empty());
    MESSAGE("streaming status: ", sink.status.c_str());
}

TEST_CASE("streaming sax: small buffer (96 bytes)") {
    Fixture f;
    VersionSink sink;
    char buf[96];
    note::SaxStreamBuf sbuf(buf);
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"card.version\"}\n", sbuf, sink);
    INFO("parse error: ", err.data());
    REQUIRE(err.empty());
    CHECK(!sink.device.empty());
    MESSAGE("96-byte buf device: ", sink.device.c_str());
}

TEST_CASE("streaming sax: error response") {
    Fixture f;
    ErrorSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"note.get\",\"file\":\"nonexistent.qi\"}\n", sink);
    // Parse should succeed (it's valid JSON), but the response contains "err"
    INFO("parse error: ", err.data());
    REQUIRE(err.empty());
    CHECK(!sink.err.empty());
    MESSAGE("notecard error: ", sink.err.c_str());
}

TEST_CASE("streaming sax: sequential requests no desync") {
    Fixture f;
    for (int i = 0; i < 5; ++i) {
        VersionSink sink;
        f.transport.reset();
        auto err = streaming_request(f.transport, "{\"req\":\"card.version\"}\n", sink);
        INFO("iteration ", i, " parse error: ", err.data());
        REQUIRE(err.empty());
        CHECK(!sink.device.empty());
    }
    MESSAGE("5 sequential streaming parses completed");
}

TEST_CASE("streaming sax: interleaved with normal execute()") {
    Fixture f;

    // Normal execute
    auto rsp1 = f.nc.card.version().execute();
    REQUIRE(rsp1);

    // Streaming SAX
    VersionSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"card.version\"}\n", sink);
    REQUIRE(err.empty());

    // Normal execute again
    auto rsp2 = f.nc.card.status().execute();
    REQUIRE(rsp2);

    // Verify all three succeeded
    CHECK(!sink.device.empty());
    CHECK(!note::string_view(rsp2.status).empty());
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
