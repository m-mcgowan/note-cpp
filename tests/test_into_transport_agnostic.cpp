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

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back(static_cast<uint8_t>('\n'));
    }

    bool transmit(const uint8_t*, size_t) override { return true; }

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
