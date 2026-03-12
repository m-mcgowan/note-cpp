/// @file test_serial.cpp
/// Integration tests exercising the Notecard over serial UART.
///
/// Tests cover:
///   - Basic request/response (card.version, card.status)
///   - Configuration round-trip (hub.set + hub.get)
///   - Note lifecycle (note.add, note.get with typed body, note.changes)
///   - Environment variables (env.default set + get)
///   - Binary data transfer (card.binary put + get with COBS encoding)
///   - Error handling (Notecard error surfacing)

#include <doctest.h>
#include <note/notecard.hpp>
#include <note/error.hpp>
#include <note/api_context.hpp>
#include <note/body.hpp>
#include <note/backends/cjson.hpp>
#include <note/transport/serial.hpp>
#include "hal_serial.hpp"
#include "../include/cobs.hpp"
#include "../include/md5.hpp"

// Use UART1 for Notecard (UART0 is the USB console).
static HardwareSerial NotecardUart(1);

namespace {

struct Fixture {
    Esp32SerialHal hal{NotecardUart};
    note::transport::NotecardSerial transport{hal};
    note::backends::CjsonBackend backend;
    note::Notecard nc{backend, transport};
    note::Api api{nc};
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
    auto r = f.api.cardVersion().execute();
    if (!r) { INFO(to_string(r.error())); }
    REQUIRE(r);
    CHECK(!note::string_view(r.device).empty());
    CHECK(!note::string_view(r.version).empty());
    MESSAGE("device: ", r.device);
    MESSAGE("version: ", r.version);
}

TEST_CASE("card.status returns operational state") {
    Fixture f;
    auto r = f.api.cardStatus().execute();
    if (!r) { INFO(to_string(r.error())); }
    REQUIRE(r);
    CHECK(!note::string_view(r.status).empty());
    MESSAGE("status: ", r.status);
    MESSAGE("storage: ", r.storage, "%");
}

// ─── Configuration round-trip ───────────────────────────────────────────────

TEST_CASE("hub.set + hub.get round-trip") {
    Fixture f;
    auto set_r = f.api.hubSet()
        .product("com.example.integration-test")
        .execute();
    if (!set_r) { INFO(to_string(set_r.error())); }
    REQUIRE(set_r);

    auto get_r = f.api.hubGet().execute();
    if (!get_r) { INFO(to_string(get_r.error())); }
    REQUIRE(get_r);
    CHECK(note::string_view(get_r.product) == "com.example.integration-test");
}

// ─── Note lifecycle ─────────────────────────────────────────────────────────

TEST_CASE("note.add sends a note") {
    Fixture f;
    auto r = f.api.noteAdd()
        .file("integration-test.qo")
        .execute();
    if (!r) { INFO(to_string(r.error())); }
    REQUIRE(r);
}

TEST_CASE("note.add + note.get body round-trip") {
    Fixture f;
    const char* file = "integration-body.qi";

    SensorData sent{.temperature = 23.5f, .humidity = 65};
    auto add_r = f.api.noteAdd()
        .file(file)
        .body(sent)
        .execute();
    if (!add_r) { INFO(to_string(add_r.error())); }
    REQUIRE(add_r);

    auto get_r = f.api.noteGet().delete_()
        .file(file)
        .execute();
    if (!get_r) { INFO(to_string(get_r.error())); }
    REQUIRE(get_r);

    SensorData received = get_r.bodyAs<SensorData>();
    CHECK(received.temperature == doctest::Approx(sent.temperature));
    CHECK(received.humidity == sent.humidity);
}

TEST_CASE("note.changes tracks additions") {
    Fixture f;
    const char* file = "integration-changes.qi";
    const char* tracker = "integration-test";

    // Reset tracker
    f.api.noteChanges().get()
        .file(file)
        .tracker(tracker)
        .start(true)
        .execute();

    // Add a note
    f.api.noteAdd().file(file).execute();

    // Check for changes
    auto r = f.api.noteChanges().get()
        .file(file)
        .tracker(tracker)
        .execute();
    if (!r) { INFO(to_string(r.error())); }
    REQUIRE(r);
    CHECK(r.changes > 0);
    MESSAGE("changes: ", r.changes, " total: ", r.total);

    // Clean up — pop the note we added
    f.api.noteGet().delete_().file(file).execute();
}

// ─── Environment variables ──────────────────────────────────────────────────

TEST_CASE("env.default set + get round-trip") {
    Fixture f;

    // Set an env var
    auto set_r = f.api.envDefault().set()
        .name("_integration_test_var")
        .text("hello-from-note-cpp")
        .execute();
    if (!set_r) { INFO(to_string(set_r.error())); }
    REQUIRE(set_r);

    // Read it back via env.get
    auto get_r = f.nc.request("env.get", [](note::JsonBuilder& b) {
        b.add("name", "_integration_test_var");
    });
    REQUIRE(get_r);
    auto text = (*get_r)->get_string("text");
    CHECK(note::string_view(text) == "hello-from-note-cpp");

    // Clean up
    f.api.envDefault().delete_()
        .name("_integration_test_var")
        .execute();
}

// ─── Binary data transfer ───────────────────────────────────────────────────

TEST_CASE("card.binary put + get round-trip") {
    Fixture f;

    // 1. Clear any existing binary data
    f.api.cardBinary().delete_().execute();

    // 2. Check available space
    auto status_r = f.api.cardBinary().get().execute();
    if (!status_r) { INFO(to_string(status_r.error())); }
    REQUIRE(status_r);
    REQUIRE(status_r.max > 0);
    MESSAGE("binary max: ", status_r.max, " bytes");

    // 3. Prepare test data
    const uint8_t test_data[] = "Hello from note-cpp binary test!";
    const size_t data_len = sizeof(test_data) - 1;  // exclude null terminator
    REQUIRE(static_cast<int32_t>(data_len) <= status_r.max);

    // COBS-encode the data
    std::vector<uint8_t> cobs_buf(cobs_encoded_size(data_len));
    size_t cobs_len = cobs_encode(test_data, data_len, cobs_buf.data());

    // Compute MD5 of unencoded data
    std::string md5 = md5_hex(test_data, data_len);

    // 4. Send card.binary.put JSON request
    auto put_r = f.api.cardBinaryPut()
        .cobs(static_cast<int32_t>(cobs_len))
        .status(md5)
        .execute();
    if (!put_r) { INFO(to_string(put_r.error())); }
    REQUIRE(put_r);

    // 5. Send the raw COBS-encoded binary data via the transport
    // After card.binary.put, the Notecard expects raw bytes on the wire.
    // Add a newline terminator as the protocol requires.
    cobs_buf.push_back('\n');
    bool tx_ok = f.hal.transmit(cobs_buf.data(), cobs_buf.size());
    REQUIRE(tx_ok);

    // Small delay for Notecard to process
    f.hal.delay(250);

    // 6. Verify data was stored
    auto verify_r = f.api.cardBinary().get().execute();
    if (!verify_r) { INFO(to_string(verify_r.error())); }
    REQUIRE(verify_r);
    CHECK(verify_r.length == static_cast<int32_t>(data_len));
    CHECK(verify_r.cobs == static_cast<int32_t>(cobs_len));
    MESSAGE("stored: length=", verify_r.length, " cobs=", verify_r.cobs);

    // 7. Retrieve the data back with card.binary.get
    auto get_r = f.api.cardBinaryGet()
        .cobs(verify_r.cobs)
        .length(verify_r.length)
        .execute();
    if (!get_r) { INFO(to_string(get_r.error())); }
    REQUIRE(get_r);

    // 8. Read the raw COBS bytes from the transport
    std::vector<uint8_t> rx_buf(cobs_len + 16);
    size_t total_rx = 0;
    uint32_t deadline = f.hal.millis() + 5000;
    while (total_rx < cobs_len && f.hal.millis() < deadline) {
        size_t n = f.hal.receive(rx_buf.data() + total_rx, rx_buf.size() - total_rx);
        total_rx += n;
        if (n == 0) f.hal.delay(10);
    }
    REQUIRE(total_rx >= cobs_len);

    // 9. COBS-decode and verify
    std::vector<uint8_t> decoded(data_len + 1);
    size_t decoded_len = cobs_decode(rx_buf.data(), cobs_len, decoded.data());
    REQUIRE(decoded_len == data_len);
    CHECK(memcmp(decoded.data(), test_data, data_len) == 0);

    // 10. Clean up
    f.api.cardBinary().delete_().execute();
}

// ─── Error handling ─────────────────────────────────────────────────────────

TEST_CASE("bad request returns Notecard error") {
    Fixture f;
    auto r = f.api.noteGet().delete_()
        .file("nonexistent-file.qi")
        .execute();
    CHECK(!r);
    if (!r) {
        CHECK(r.error().code == note::Error::Notecard);
        MESSAGE("error: ", to_string(r.error()));
    }
}

} // TEST_SUITE
