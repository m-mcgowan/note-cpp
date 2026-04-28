// ────────────────────────────────────────────────────────────────────────────
// note-cpp transport-agnostic API parity
// ────────────────────────────────────────────────────────────────────────────
//
// Reading this file should make the architectural model self-evident:
//
//   ┌────────────────────────────────────────────────────────────────────┐
//   │ High-level typed API (api.note.read().into(struct).execute(), ...) │  ← § 1, § 2
//   ├────────────────────────────────────────────────────────────────────┤
//   │ Low-level raw JSON API (nc.transact(json, buf), nc.send(json))     │  ← § 3, § 4
//   ├────────────────────────────────────────────────────────────────────┤
//   │ JSON layer — turns response bytes into typed values                │
//   │   • tree-mode  (JsonReader via JsonBackend; supports `body()`)     │
//   │   • sink-mode  (SAX events into Rsp::Sink; supports `.into()`)     │
//   ├────────────────────────────────────────────────────────────────────┤
//   │ ITransport / Protocol / Hal — wire frames, retries, byte conduit   │
//   └────────────────────────────────────────────────────────────────────┘
//
// Both the high-level and low-level API surfaces are transport-agnostic:
// the same call yields equivalent results no matter which transport the
// Notecard was constructed against. The buffered/streaming distinction is
// not about transport at all — it is about which JSON-layer strategy the
// Notecard runs internally to turn response bytes into typed values.
//
// Each section below pairs a streaming-Notecard test against a
// buffered-Notecard test exercising the same surface. CI fails if the two
// ever diverge.

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

// ─── Shared test fixtures ───────────────────────────────────────────────────

namespace {

struct SensorReading {
    float temperature{};
    int32_t humidity{};
    NOTE_FIELDS(temperature, humidity)
};

constexpr const char* kCannedResponse =
    R"({"payload":"dGVzdA==","time":1234,"body":{"temperature":23.5,"humidity":65}})";

// MockHal mirrors the one in test_transport_streaming.cpp — just enough to
// drive a StreamingTransport with a single canned response and capture the
// bytes the protocol transmits.
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

// Locate `"body":{ ... }` and return the slice (including braces). Returns
// empty view if no body object is present.
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


// ────────────────────────────────────────────────────────────────────────────
// § 1 — High-level typed API: `.into(T&)` populates the user's struct.
//
// Same canned response over both transports. The destination struct must
// fill identically; if it doesn't, the buffered execute path forgot to
// dispatch body events into the body handler.
// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("§1 typed API: .into() populates struct on streaming transport") {
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

TEST_CASE("§1 typed API: .into() populates struct on buffered transport") {
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
    CHECK(reading.temperature == doctest::Approx(23.5f));
    CHECK(reading.humidity == 65);
}


// ────────────────────────────────────────────────────────────────────────────
// § 2 — High-level typed API: `.body(T&)` emits the same wire JSON.
//
// Writer-side analog of `.into()`. Both transports must serialise the
// user's struct into the request body byte-for-byte identically.
// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("§2 typed API: .body() — streaming and buffered emit identical body JSON") {
    SensorReading sent{};
    sent.temperature = 24.75f;
    sent.humidity = 55;

    MockHal stream_hal;
    stream_hal.queue_response("{}");
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
    auto streaming_body = std::string(extract_body_json(
        note::string_view(stream_hal.last_transmitted)));
    REQUIRE_FALSE(streaming_body.empty());

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

    CHECK(streaming_body == buffered_body);
}


// ────────────────────────────────────────────────────────────────────────────
// § 3 — Low-level raw JSON API: `nc.transact(json, buf)`.
//
// Pre-formatted request, response copied into the caller's buffer. Both
// transports must (a) emit the same JSON bytes on the wire and (b) hand
// the caller back equivalent response bytes.
// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("§3 raw transact(json, buf) — same wire bytes, same response on both transports") {
    constexpr const char* kRawRequest  = R"({"req":"card.version"})";
    constexpr const char* kRawResponse = R"({"version":"notecard-7.5.2","device":"dev:0"})";

    // ── Streaming side: hits transact_raw via the new ITransport overload.
    MockHal stream_hal;
    stream_hal.queue_response(kRawResponse);
    note::StreamingTransport stream_transport(stream_hal);
    note::Notecard stream_nc(stream_transport, note::Allocator{});
    char stream_buf[256];
    auto stream_rv = stream_nc.transact(kRawRequest, note::span<char>(stream_buf, sizeof(stream_buf)));
    REQUIRE(stream_rv.has_value());

    // ── Buffered side: lambda echoes the canned response.
    note::backends::BufferJsonBackend<1024, 64> backend;
    std::string buffered_seen_request;
    note::test::CallbackTransport buffered_transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            buffered_seen_request = std::string(req);
            return note::string_view(kRawResponse);
        });
    note::Notecard buffered_nc(backend, buffered_transport);
    char buffered_buf[256];
    auto buffered_rv = buffered_nc.transact(kRawRequest,
                                            note::span<char>(buffered_buf, sizeof(buffered_buf)));
    REQUIRE(buffered_rv.has_value());

    // Wire JSON parity: streaming emits raw request directly; buffered
    // wraps via CallbackTransport's lambda. The bytes seen on each side
    // must contain the same request envelope.
    CHECK(stream_hal.last_transmitted.find(kRawRequest) != std::string::npos);
    CHECK(buffered_seen_request == kRawRequest);

    // Response parity: caller sees the same bytes either way.
    CHECK(std::string(stream_rv->data(), stream_rv->size()) == kRawResponse);
    CHECK(std::string(buffered_rv->data(), buffered_rv->size()) == kRawResponse);
}


// ────────────────────────────────────────────────────────────────────────────
// § 4 — Low-level raw JSON API: `nc.send(json)` (fire-and-forget).
//
// Same JSON byte-for-byte on the wire regardless of transport.
// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("§4 raw send(json) — same wire bytes on both transports") {
    constexpr const char* kCommand = R"({"cmd":"card.attn","mode":"disarm"})";

    // ── Streaming side.
    MockHal stream_hal;
    stream_hal.queue_response("{}");
    note::StreamingTransport stream_transport(stream_hal);
    note::Notecard stream_nc(stream_transport, note::Allocator{});
    auto stream_send = stream_nc.send(kCommand);
    REQUIRE(stream_send.has_value());

    // ── Buffered side.
    std::string buffered_seen;
    note::backends::BufferJsonBackend<512, 64> backend;
    note::test::CallbackTransport buffered_transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            buffered_seen = std::string(req);
            return note::string_view("{}");
        },
        [&](note::string_view req) -> note::Result<void> {
            buffered_seen = std::string(req);
            return {};
        });
    note::Notecard buffered_nc(backend, buffered_transport);
    auto buffered_send = buffered_nc.send(kCommand);
    REQUIRE(buffered_send.has_value());

    // Same envelope, same bytes — modulo whatever framing each wire layer
    // adds before/after (CRC field, line terminator). The request payload
    // itself is byte-identical.
    CHECK(stream_hal.last_transmitted.find(kCommand) != std::string::npos);
    CHECK(buffered_seen == kCommand);
}
