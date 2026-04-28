// Verifies that `.into(T&)` populates the destination struct identically
// regardless of which transport (streaming or buffered) the Notecard is
// constructed against. Conceptually `.into()` is a high-level API contract
// — the user should not need to know which transport is in play.

#include <doctest.h>

#include <note/api.hpp>
#include <note/allocator.hpp>
#include <note/backends/buffer.hpp>
#include <note/streaming_transport.hpp>
#include <note/transport.hpp>
#include <note/transport_hal.hpp>

#include "test_notecard_factory.hpp"

#include <cstring>
#include <deque>
#include <string>

namespace {

struct SensorReading {
    float temperature{};
    int32_t humidity{};
    NOTE_FIELDS(temperature, humidity)
};

constexpr const char* kCannedResponse =
    R"({"payload":"dGVzdA==","time":1234,"body":{"temperature":23.5,"humidity":65}})";

// MockHal mirrors the one in test_transport_streaming.cpp — just enough to
// drive a StreamingTransport with a single canned response.
class MockHal : public note::Hal {
public:
    std::deque<uint8_t> rx;
    std::string last_transmitted;

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back(static_cast<uint8_t>('\n'));
    }

    bool transmit(const uint8_t* data, size_t len) override {
        last_transmitted.append(reinterpret_cast<const char*>(data), len);
        return true;
    }

    note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
        if (rx.empty())
            return note::make_error(note::Error::ResponseLost,
                                    note::Cause::Timeout, "no data");
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        return n;
    }

    bool reset() override { return true; }
    bool write_line_terminator() override { return true; }
    void delay(uint32_t) override {}
    uint32_t millis() override { return 0; }
};

} // namespace

TEST_CASE(".into(): streaming transport populates target struct") {
    MockHal hal;
    hal.queue_response(kCannedResponse);

    note::StreamingTransport transport(hal);
    auto nc = note::test::make_test_notecard(transport, note::Allocator{});
#if __cplusplus >= 202002L
    note::Api<> api(nc);
#else
    note::Api api(nc);
#endif

    SensorReading reading{};
    auto rsp = api.note.read("test.db").noteId("x").into(reading).execute();
    REQUIRE(rsp.has_value());
    CHECK(rsp.time == 1234);

    CHECK(reading.temperature == doctest::Approx(23.5f));
    CHECK(reading.humidity == 65);
}

TEST_CASE(".into(): buffered transport populates target struct identically") {
    note::backends::BufferJsonBackend<1024, 64> backend;
    note::test::CallbackTransport transport(
        [&](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view(kCannedResponse);
        });

    auto nc = note::test::make_test_notecard(backend, transport);
#if __cplusplus >= 202002L
    note::Api<> api(nc);
#else
    note::Api api(nc);
#endif

    SensorReading reading{};
    auto rsp = api.note.read("test.db").noteId("x").into(reading).execute();
    REQUIRE(rsp.has_value());
    CHECK(rsp.time == 1234);

    // The high-level API contract: `.into()` populates the destination
    // struct, regardless of transport. Today's buffered execute path
    // ignores body_handler_factory_, so these checks fail until the fix.
    CHECK(reading.temperature == doctest::Approx(23.5f));
    CHECK(reading.humidity == 65);
}

// ─── Sending bodies: `.body(struct)` parity across transports ─────────────────
//
// `.body(struct)` is the writer-side analog of `.into()`. It must serialise
// the user's struct into the request `body` object identically regardless of
// transport. The streaming path emits via StreamingJsonBuilder (direct to
// wire); the buffered path emits via the JsonBackend's builder (into a
// buffer). Both paths route the same `req.build(b)` callback through their
// builder, so the resulting wire JSON should be byte-equivalent.

namespace {

// Locate `"body":{ ... }` and return the slice (including braces). Returns
// empty view if no body object is present in the input.
note::string_view extract_body_json(note::string_view req) {
    auto pos = req.find("\"body\":");
    if (pos == note::string_view::npos) return {};
    pos += 7;  // length of `"body":`
    if (pos >= req.size() || req[pos] != '{') return {};
    int depth = 0;
    for (size_t i = pos; i < req.size(); ++i) {
        char c = req[i];
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0)
                return req.substr(pos, i - pos + 1);
        }
    }
    return {};
}

} // namespace

TEST_CASE(".body(): streaming and buffered emit identical body JSON") {
    SensorReading sent{};
    sent.temperature = 24.75f;
    sent.humidity = 55;

    // ── Streaming side: capture the bytes the StreamingJsonBuilder transmits.
    MockHal stream_hal;
    stream_hal.queue_response("{}");  // any well-formed empty response
    note::StreamingTransport stream_transport(stream_hal);

    {
        auto nc = note::test::make_test_notecard(stream_transport, note::Allocator{});
#if __cplusplus >= 202002L
        note::Api<> api(nc);
#else
        note::Api api(nc);
#endif
        auto rsp = api.note.update("test.db", "x").body(sent).execute();
        REQUIRE(rsp.has_value());
    }

    std::string streaming_body{
        extract_body_json(note::string_view(stream_hal.last_transmitted))};
    REQUIRE_FALSE(streaming_body.empty());

    // ── Buffered side: capture the bytes the JsonBackend builder produces.
    std::string buffered_request;
    note::backends::BufferJsonBackend<1024, 64> backend;
    note::test::CallbackTransport buffered(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            buffered_request = std::string(req);
            return note::string_view("{}");
        });

    {
        auto nc = note::test::make_test_notecard(backend, buffered);
#if __cplusplus >= 202002L
        note::Api<> api(nc);
#else
        note::Api api(nc);
#endif
        auto rsp = api.note.update("test.db", "x").body(sent).execute();
        REQUIRE(rsp.has_value());
    }

    auto buffered_body = std::string(extract_body_json(note::string_view(
        buffered_request.data(), buffered_request.size())));
    REQUIRE_FALSE(buffered_body.empty());

    // Same struct → same body JSON, regardless of transport.
    CHECK(streaming_body == buffered_body);
}

