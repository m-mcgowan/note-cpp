/// @file test_serial.cpp
/// Serial-specific integration tests — binary transfer, ATTN payload, streaming SAX.
///
/// Transport-agnostic tests are in test_notecard_api.cpp (shared via symlink).
/// This file contains only tests that require serial transport features.

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

} // namespace

TEST_SUITE("serial") {

// ─── ATTN payload ───────────────────────────────────────────────────────────

TEST_CASE("card.attn payload without sleep") {
    Fixture f;
    auto& nc = f.nc;

    auto req = nc.card.attn().request();
    req.payload = "dGVzdC1wYXlsb2FkLW5vLXNsZWVw";
    auto rsp = req.execute();
    if (!rsp) { MESSAGE("attn error: ", note::to_string(rsp.error())); }
    REQUIRE(rsp);

    auto retrieve = nc.card.attn().retrieve().execute();
    if (!retrieve) { INFO(note::to_string(retrieve.error())); }
    REQUIRE(retrieve);

    MESSAGE("payload: ", retrieve.payload.data());
    CHECK(note::string_view(retrieve.payload) == "dGVzdC1wYXlsb2FkLW5vLXNsZWVw");

    nc.card.attn().disarm().execute();
}

TEST_CASE("card.attn payload with sleep timer") {
    Fixture f;
    auto& nc = f.nc;

    auto sleep_req = nc.card.attn().sleep();
    sleep_req.seconds(note::Seconds{1});
    sleep_req.payload("dGVzdC1wYXlsb2FkLXdpdGgtc2xlZXA=");
    auto sleep_rsp = sleep_req.execute();
    if (!sleep_rsp) { MESSAGE("sleep error: ", note::to_string(sleep_rsp.error())); }
    REQUIRE(sleep_rsp);

    f.hal.delay(2000);

    auto retrieve = nc.card.attn().retrieve().execute();
    if (!retrieve) { MESSAGE("retrieve error: ", note::to_string(retrieve.error())); }
    REQUIRE(retrieve);

    CHECK(retrieve.time.value() != 0);
    CHECK(note::string_view(retrieve.payload) == "dGVzdC1wYXlsb2FkLXdpdGgtc2xlZXA=");

    nc.card.attn().disarm().execute();
}

// ─── Binary data transfer ───────────────────────────────────────────────────

namespace {

void binary_round_trip(Fixture& f, const uint8_t* data, size_t data_len, const char* label) {
    auto& nc = f.nc;
    INFO("payload: ", label, " (", data_len, " bytes)");

    auto put_rsp = nc.card.binary.put()
        .data(data, data_len)
        .execute();
    if (!put_rsp) { MESSAGE("put error: ", note::to_string(put_rsp.error())); }
    REQUIRE(put_rsp);

    auto status_rsp = nc.binary.status().execute();
    REQUIRE(status_rsp);
    REQUIRE(status_rsp.length > 0);

    std::vector<uint8_t> dst(data_len);
    auto get_rsp = nc.card.binary.get()
        .into(dst.data(), dst.size())
        .length(status_rsp.length)
        .execute();
    if (!get_rsp) { MESSAGE("get error: ", note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);

    CHECK(memcmp(dst.data(), data, data_len) == 0);
}

} // namespace

TEST_CASE("card.binary put + get — text payload") {
    Fixture f;
    const uint8_t data[] = "Hello from note-cpp binary test!";
    binary_round_trip(f, data, sizeof(data) - 1, "text");
}

TEST_CASE("card.binary put + get — data with zero bytes") {
    Fixture f;
    uint8_t data[64];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = static_cast<uint8_t>(i % 5 == 0 ? 0 : i);
    binary_round_trip(f, data, sizeof(data), "zeros");
}

TEST_CASE("card.binary put + get — 512-byte payload") {
    Fixture f;
    uint8_t data[512];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    binary_round_trip(f, data, sizeof(data), "512B");
}

// ─── Streaming SAX parser over real UART ────────────────────────────────────

namespace {

struct VersionSink : public note::JsonSink {
    std::string device, version;
    void on_string(note::string_view key, note::string_view val) override {
        if (key == "device")  device.assign(val.data(), val.size());
        if (key == "version") version.assign(val.data(), val.size());
    }
};

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

struct ErrorSink : public note::JsonSink {
    std::string err;
    void on_string(note::string_view key, note::string_view val) override {
        if (key == "err") err.assign(val.data(), val.size());
    }
};

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
    auto rsp = f.nc.card.version().execute();
    REQUIRE(rsp);
    std::string expected_device(rsp.device.data(), rsp.device.size());

    VersionSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"card.version\"}\n", sink);
    INFO("parse error: ", err.data());
    REQUIRE(err.empty());
    CHECK(sink.device == expected_device);
    CHECK(sink.version.substr(0, 9) == "notecard-");
}

TEST_CASE("streaming sax: card.status") {
    Fixture f;
    StatusSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"card.status\"}\n", sink);
    INFO("parse error: ", err.data());
    REQUIRE(err.empty());
    CHECK(!sink.status.empty());
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
}

TEST_CASE("streaming sax: error response") {
    Fixture f;
    ErrorSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"note.get\",\"file\":\"nonexistent.qi\"}\n", sink);
    INFO("parse error: ", err.data());
    REQUIRE(err.empty());
    CHECK(!sink.err.empty());
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
}

TEST_CASE("streaming sax: interleaved with normal execute()") {
    Fixture f;
    auto rsp1 = f.nc.card.version().execute();
    REQUIRE(rsp1);

    VersionSink sink;
    f.transport.reset();
    auto err = streaming_request(f.transport, "{\"req\":\"card.version\"}\n", sink);
    REQUIRE(err.empty());

    auto rsp2 = f.nc.card.status().execute();
    REQUIRE(rsp2);

    CHECK(!sink.device.empty());
    CHECK(!note::string_view(rsp2.status).empty());
}

} // TEST_SUITE("serial")

#endif // NOTECARD_TEST_SERIAL
