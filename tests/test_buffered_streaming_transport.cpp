// Tests for note::BufferedStreamingTransport — IBufferedTransport adapter
// over a StreamingTransport. Verifies that the adapter:
//
//  - delegates transact/send to StreamingTransport's raw-passthrough path
//  - copies the response bytes into the caller-supplied buffer
//  - forwards reset/abort/millis/delay
//  - forwards binary write/read
//
// Uses a TransportHal mock (same shape as test_transport_streaming.cpp) to
// drive a real StreamingTransport, so we exercise the actual buffered →
// streaming bridge end to end.

#include <doctest.h>

#include <note/buffered_transport.hpp>
#include <note/streaming_transport.hpp>
#include <note/transport_hal.hpp>

#include <deque>
#include <string>

using namespace note;

namespace {

class MockHal : public TransportHal {
public:
    std::deque<uint8_t> rx;
    std::string last_transmitted;
    int transmit_count = 0;
    bool transmit_fails = false;

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back(static_cast<uint8_t>('\n'));
    }

    bool transmit(const uint8_t* data, size_t len) override {
        ++transmit_count;
        if (transmit_fails) return false;
        last_transmitted.append(reinterpret_cast<const char*>(data), len);
        return true;
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t /*timeout_ms*/) override {
        if (rx.empty())
            return make_error(Error::ResponseLost, Cause::Timeout, "no data");
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        return n;
    }

    bool reset() override { ++reset_count; return true; }
    bool write_line_terminator() override { last_transmitted += "\r\n"; return true; }
    void delay(uint32_t ms) override { last_delay_ms = ms; }
    uint32_t millis() override { return now_ms; }

    int reset_count = 0;
    uint32_t last_delay_ms = 0;
    uint32_t now_ms = 12345;
};

}  // namespace

TEST_CASE("BufferedStreamingTransport: transact round-trips JSON into caller's buffer") {
    MockHal hal;
    hal.queue_response("{\"text\":\"hello\",\"time\":42}");

    StreamingTransport streaming(hal, /*max_retries=*/0, /*retry_delay_ms=*/0);
    char buf[256];
    BufferedStreamingTransport buffered(streaming, buf, sizeof(buf));

    auto r = static_cast<IBufferedTransport&>(buffered)
                 .transact("{\"req\":\"env.get\"}", 1000);
    REQUIRE(r);
    CHECK(*r == "{\"text\":\"hello\",\"time\":42}");
    CHECK(hal.last_transmitted.find("env.get") != std::string::npos);
}

TEST_CASE("BufferedStreamingTransport: response is copied into buffer") {
    MockHal hal;
    hal.queue_response("{\"k\":\"v\"}");

    StreamingTransport streaming(hal, 0, 0);
    char buf[64];
    BufferedStreamingTransport buffered(streaming, buf, sizeof(buf));

    auto r = buffered.transact("{\"req\":\"x\"}", 1000);
    REQUIRE(r);
    // The string_view points into the caller's buffer.
    CHECK(r->data() >= buf);
    CHECK(r->data() < buf + sizeof(buf));
}

TEST_CASE("BufferedStreamingTransport: send forwards to send_raw") {
    MockHal hal;
    StreamingTransport streaming(hal, 0, 0);
    char buf[64];
    BufferedStreamingTransport buffered(streaming, buf, sizeof(buf));

    auto r = buffered.send("{\"cmd\":\"hub.sync\"}");
    REQUIRE(r);
    CHECK(hal.last_transmitted.find("hub.sync") != std::string::npos);
}

TEST_CASE("BufferedStreamingTransport: reset / abort / millis / delay forwarded") {
    MockHal hal;
    hal.now_ms = 9999;
    StreamingTransport streaming(hal, 0, 0);
    char buf[8];
    BufferedStreamingTransport buffered(streaming, buf, sizeof(buf));

    CHECK(buffered.millis() == 9999);

    buffered.delay(50);
    CHECK(hal.last_delay_ms == 50);

    buffered.reset();
    CHECK(hal.reset_count == 1);

    // abort() is a no-op in StreamingTransport — just ensure it compiles
    // and returns without throwing.
    buffered.abort();
}

TEST_CASE("BufferedStreamingTransport: binary write/read forwarded") {
    MockHal hal;
    StreamingTransport streaming(hal, 0, 0);
    char buf[16];
    BufferedStreamingTransport buffered(streaming, buf, sizeof(buf));

    const uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto w = buffered.write(bytes, sizeof(bytes));
    REQUIRE(w);
    REQUIRE(hal.last_transmitted.size() == sizeof(bytes));
    CHECK(static_cast<uint8_t>(hal.last_transmitted[0]) == 0xDE);
    CHECK(static_cast<uint8_t>(hal.last_transmitted[3]) == 0xEF);

    hal.rx.push_back(0xCA);
    hal.rx.push_back(0xFE);
    uint8_t rb[2] = {};
    auto r = buffered.read(rb, sizeof(rb), 100);
    REQUIRE(r);
    CHECK(*r == 2);
    CHECK(rb[0] == 0xCA);
    CHECK(rb[1] == 0xFE);
}

TEST_CASE("BufferedStreamingTransport: set_buffer swaps response buffer") {
    MockHal hal;
    hal.queue_response("{\"k\":\"v\"}");
    StreamingTransport streaming(hal, 0, 0);

    char small[8];
    BufferedStreamingTransport buffered(streaming, small, sizeof(small));

    char large[256];
    buffered.set_buffer(span<char>(large, sizeof(large)));

    auto r = buffered.transact("{\"req\":\"x\"}", 1000);
    REQUIRE(r);
    CHECK(r->data() >= large);
    CHECK(r->data() < large + sizeof(large));
}
