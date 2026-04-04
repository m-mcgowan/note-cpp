// Tests for StreamingTransport over a MockHal.
//
// The new transport architecture splits:
//   - TransportHal — pure hardware abstraction (transmit, read, reset, etc.)
//   - StreamingTransport — protocol logic (retry, CRC, JSON framing) over a HAL
//   - IStreamingTransport — type-erased interface
//
// Tests exercise transact() (build request via BuildFn, SAX-parse response into
// a JsonSink) and send() (fire-and-forget).

#include "catch.hpp"

#include <note/streaming_transport.hpp>
#include <note/transport_hal.hpp>
#include <note/api.hpp>
#include <note/allocator.hpp>

#include "test_notecard_factory.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace note;

// ---------------------------------------------------------------------------
// CollectorSink — simple JsonSink that records fields for test assertions.
// ---------------------------------------------------------------------------

struct CollectorSink : JsonSink {
    struct Entry {
        std::string key;
        std::string value;
        enum { String, Number, Bool, Null, ObjBegin, ObjEnd } type;
    };
    std::vector<Entry> entries;
    int reset_count = 0;

    void on_string(string_view key, string_view value) override {
        entries.push_back({std::string(key), std::string(value), Entry::String});
    }
    void on_number(string_view key, string_view raw) override {
        entries.push_back({std::string(key), std::string(raw), Entry::Number});
    }
    void on_bool(string_view key, bool value) override {
        entries.push_back({std::string(key), value ? "true" : "false", Entry::Bool});
    }
    void on_null(string_view key) override {
        entries.push_back({std::string(key), "null", Entry::Null});
    }
    void on_object_begin(string_view key) override {
        entries.push_back({std::string(key), "", Entry::ObjBegin});
    }
    void on_object_end(string_view key) override {
        entries.push_back({std::string(key), "", Entry::ObjEnd});
    }
    void reset() override {
        entries.clear();
        ++reset_count;
    }

    // Convenience: find the value for a top-level key.
    std::string get(const std::string& key) const {
        for (auto& e : entries)
            if (e.key == key) return e.value;
        return {};
    }
    bool has(const std::string& key) const {
        for (auto& e : entries)
            if (e.key == key) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// MockHal — minimal TransportHal for testing StreamingTransport.
//
// Uses a byte deque for read(). Transmit captures bytes. Reset always succeeds.
// ---------------------------------------------------------------------------

class MockHal : public TransportHal {
public:
    std::deque<uint8_t> rx;              // bytes returned by read()
    std::string last_transmitted;        // last transmit payload
    int transmit_count = 0;
    bool transmit_fails = false;

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        // Frame terminator: \n is the protocol end-of-packet marker.
        // note-c checks only for \n (n_serial.c:269, n_i2c.c:404).
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

    bool reset() override { return true; }

    bool write_line_terminator() override {
        last_transmitted += "\r\n";
        return true;
    }

    void delay(uint32_t /*ms*/) override {}
    uint32_t millis() override { return 0; }
};

// Helper: get IStreamingTransport& from StreamingTransport to access
// the convenience template overloads (transact(F&&, ...), send(F&&)).
static IStreamingTransport& iface(StreamingTransport& t) { return t; }


// ---------------------------------------------------------------------------
// transact: basic response parsed into sink
// ---------------------------------------------------------------------------

TEST_CASE("transact: basic response parsed via SAX into sink") {
    MockHal hal;
    hal.queue_response("{\"ok\":true}");

    StreamingTransport transport(hal, /*max_retries=*/1, /*retry_delay_ms=*/0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "hub.status"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.has("ok"));
    REQUIRE(sink.get("ok") == "true");
}

TEST_CASE("transact: response with multiple fields") {
    MockHal hal;
    hal.queue_response("{\"status\":\"connected\",\"count\":42}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "hub.status"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.get("status") == "connected");
    REQUIRE(sink.get("count") == "42");
}

TEST_CASE("transact: empty object response") {
    MockHal hal;
    hal.queue_response("{}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "hub.status"); }, sink, 5000);
    REQUIRE(r.has_value());
    // Only root object begin/end, no value entries
    bool has_value_entry = false;
    for (auto& e : sink.entries) {
        if (e.type == CollectorSink::Entry::String ||
            e.type == CollectorSink::Entry::Number ||
            e.type == CollectorSink::Entry::Bool) {
            has_value_entry = true;
        }
    }
    REQUIRE_FALSE(has_value_entry);
}


// ---------------------------------------------------------------------------
// transact: read error triggers retry
// ---------------------------------------------------------------------------

TEST_CASE("transact: read error triggers retry and returns failure") {
    MockHal hal;
    // No data queued -> read fails -> retry -> still no data -> fail
    StreamingTransport transport(hal, /*max_retries=*/1, /*retry_delay_ms=*/0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE_FALSE(r.has_value());
}


// ---------------------------------------------------------------------------
// transact: transmit failure retries and ultimately fails
// ---------------------------------------------------------------------------

TEST_CASE("transact: transmit failure retries and returns SendFailed") {
    MockHal hal;
    hal.transmit_fails = true;
    hal.queue_response("{\"ok\":true}");

    StreamingTransport transport(hal, /*max_retries=*/1, /*retry_delay_ms=*/0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::SendFailed);
}


// ---------------------------------------------------------------------------
// transact: chunked read across multiple read() calls
// ---------------------------------------------------------------------------

TEST_CASE("transact: chunked read works correctly") {
    // Queue bytes normally — the mock returns them in whatever chunk size
    // the transport requests, which exercises multi-read.
    MockHal hal;
    hal.queue_response("{\"v\":1}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.get("v") == "1");
}


// ---------------------------------------------------------------------------
// transact: reset on first use
// ---------------------------------------------------------------------------

TEST_CASE("transact: reset on first use succeeds") {
    MockHal hal;
    hal.queue_response("{\"first\":true}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.get("first") == "true");

    // Second call should work without issues
    hal.last_transmitted.clear();
    hal.queue_response("{\"second\":true}");
    CollectorSink sink2;
    auto r2 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test2"); }, sink2, 5000);
    REQUIRE(r2.has_value());
    REQUIRE(sink2.get("second") == "true");
}


// ---------------------------------------------------------------------------
// transact: reset failure returns NotReady
// ---------------------------------------------------------------------------

TEST_CASE("transact: reset failure returns NotReady") {
    // Use a HAL that fails reset by subclassing MockHal.
    struct FailResetHal : MockHal {
        bool reset() override { return false; }
    };

    FailResetHal hal;
    hal.queue_response("{\"ok\":true}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::NotReady);
}


// ---------------------------------------------------------------------------
// transact: single attempt returns error on failure (no retry)
// ---------------------------------------------------------------------------

TEST_CASE("transact: single attempt returns error without retry") {
    MockHal hal;
    // No data queued — single attempt fails immediately.
    StreamingTransport transport(hal);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::ResponseLost);
}


// ---------------------------------------------------------------------------
// send: fire-and-forget
// ---------------------------------------------------------------------------

TEST_CASE("send: fire-and-forget succeeds") {
    MockHal hal;
    StreamingTransport transport(hal, 1, 0);

    auto r = iface(transport).send(
        [](JsonBuilder& b) { b.add("req", "card.led"); b.add("mode", 1); });
    REQUIRE(r.has_value());
    // Verify the request was transmitted (contains the JSON + terminator)
    REQUIRE(hal.last_transmitted.find("card.led") != std::string::npos);
}

TEST_CASE("send: transmit failure returns SendFailed") {
    MockHal hal;
    hal.transmit_fails = true;

    StreamingTransport transport(hal, 1, 0);

    auto r = iface(transport).send(
        [](JsonBuilder& b) { b.add("req", "card.led"); });
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::SendFailed);
}


// ---------------------------------------------------------------------------
// transact: request content is correctly streamed
// ---------------------------------------------------------------------------

TEST_CASE("transact: request is streamed as JSON to HAL") {
    MockHal hal;
    hal.queue_response("{\"ok\":true}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) {
            b.add("req", "hub.status");
            b.add("sync", true);
        }, sink, 5000);
    REQUIRE(r.has_value());

    // The transmitted data should contain the JSON request
    REQUIRE(hal.last_transmitted.find("\"req\":\"hub.status\"") != std::string::npos);
    REQUIRE(hal.last_transmitted.find("\"sync\":true") != std::string::npos);
    // Should end with closing brace + line terminator
    auto pos = hal.last_transmitted.rfind("}\r\n");
    REQUIRE(pos != std::string::npos);
}


// ---------------------------------------------------------------------------
// transact: response with string value
// ---------------------------------------------------------------------------

TEST_CASE("transact: response with string value") {
    MockHal hal;
    hal.queue_response("{\"device\":\"dev:123456\"}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "hub.status"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.get("device") == "dev:123456");
}


// ---------------------------------------------------------------------------
// write/read raw binary passthrough
// ---------------------------------------------------------------------------

TEST_CASE("write: raw binary passthrough to HAL") {
    MockHal hal;
    StreamingTransport transport(hal, 1, 0);
    // Force init so transport is ready
    hal.queue_response("{}");
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    hal.last_transmitted.clear();

    uint8_t data[] = {0x01, 0x02, 0x03};
    auto r = transport.write(data, 3);
    REQUIRE(r.has_value());
    REQUIRE(hal.last_transmitted.size() == 3);
}

TEST_CASE("read: raw binary passthrough from HAL") {
    MockHal hal;
    StreamingTransport transport(hal, 1, 0);
    // Force init
    hal.queue_response("{}");
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);

    hal.rx.push_back(0xAA);
    hal.rx.push_back(0xBB);

    uint8_t buf[4];
    auto r = transport.read(buf, sizeof(buf), 1000);
    REQUIRE(r.has_value());
    REQUIRE(*r == 2);
    REQUIRE(buf[0] == 0xAA);
    REQUIRE(buf[1] == 0xBB);
}


// ---------------------------------------------------------------------------
// reset: clears initialized state
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ChunkedMockHal — delivers bytes in configurable chunk sizes.
// Simulates serial/I2C HALs where bytes arrive incrementally.
// ---------------------------------------------------------------------------

struct ChunkedMockHal : MockHal {
    size_t max_chunk;
    explicit ChunkedMockHal(size_t chunk) : max_chunk(chunk) {}

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
        return MockHal::read(buf, std::min(max_len, max_chunk), timeout_ms);
    }
};


// ---------------------------------------------------------------------------
// Frame boundary: frame-aware read stops at \n
// ---------------------------------------------------------------------------

TEST_CASE("transact: frame-aware read at various chunk sizes") {
    for (size_t chunk : {size_t(1), size_t(2), size_t(3), size_t(7), size_t(64)}) {
        CAPTURE(chunk);
        ChunkedMockHal hal(chunk);
        hal.queue_response(R"({"status":"ok","count":42})");

        StreamingTransport transport(hal);
        CollectorSink sink;

        auto r = iface(transport).transact(
            [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
        REQUIRE(r.has_value());
        REQUIRE(sink.get("status") == "ok");
        REQUIRE(sink.get("count") == "42");
        REQUIRE(hal.rx.empty());
    }
}


// ---------------------------------------------------------------------------
// Frame boundary: consecutive transactions with chunked reads
// ---------------------------------------------------------------------------

TEST_CASE("transact: consecutive transactions at various chunk sizes") {
    for (size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(64)}) {
        CAPTURE(chunk);
        ChunkedMockHal hal(chunk);
        StreamingTransport transport(hal);

        hal.queue_response(R"({"a":1})");
        CollectorSink sink1;
        auto r1 = iface(transport).transact(
            [](JsonBuilder& b) { b.add("req", "t1"); }, sink1, 5000);
        REQUIRE(r1.has_value());
        REQUIRE(sink1.get("a") == "1");

        hal.queue_response(R"({"b":2})");
        CollectorSink sink2;
        auto r2 = iface(transport).transact(
            [](JsonBuilder& b) { b.add("req", "t2"); }, sink2, 5000);
        REQUIRE(r2.has_value());
        REQUIRE(sink2.get("b") == "2");
    }
}


// ---------------------------------------------------------------------------
// Frame boundary: parse error drains frame so next transaction is clean
// ---------------------------------------------------------------------------

TEST_CASE("transact: parse error drains frame boundary") {
    // Single-byte reads: parser reads 1 byte, fails on invalid JSON,
    // remaining bytes must be drained through \n.
    ChunkedMockHal hal(1);

    // Invalid response (not JSON) with frame terminator
    for (char c : std::string("invalid\r\n"))
        hal.rx.push_back(static_cast<uint8_t>(c));

    StreamingTransport transport(hal);

    CollectorSink sink1;
    auto r1 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "t1"); }, sink1, 5000);
    REQUIRE_FALSE(r1.has_value());
    // Frame boundary must be drained — no stale bytes
    REQUIRE(hal.rx.empty());

    // Next transaction should succeed
    hal.queue_response(R"({"ok":true})");
    CollectorSink sink2;
    auto r2 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "t2"); }, sink2, 5000);
    REQUIRE(r2.has_value());
    REQUIRE(sink2.get("ok") == "true");
}


// ---------------------------------------------------------------------------
// Frame boundary: \r\n split across reads
// ---------------------------------------------------------------------------

TEST_CASE("transact: frame boundary split across reads") {
    // Response: {"v":1}\n = 8 bytes. Various chunk sizes exercise \n
    // landing at different positions within/across reads.
    for (size_t chunk : {size_t(7), size_t(8), size_t(9), size_t(10)}) {
        CAPTURE(chunk);
        ChunkedMockHal hal(chunk);
        hal.queue_response(R"({"v":1})");

        StreamingTransport transport(hal);
        CollectorSink sink;

        auto r = iface(transport).transact(
            [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
        REQUIRE(r.has_value());
        REQUIRE(sink.get("v") == "1");
        REQUIRE(hal.rx.empty());
    }
}


// ---------------------------------------------------------------------------
// Frame boundary: \r\n framing (real Notecard sends \r\n on serial)
// ---------------------------------------------------------------------------

TEST_CASE("transact: \\r\\n framing at various chunk sizes") {
    for (size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(64)}) {
        CAPTURE(chunk);
        ChunkedMockHal hal(chunk);
        // Manually queue \r\n to match real Notecard serial behavior
        std::string resp = R"({"status":"ok"})";
        resp += "\r\n";
        for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));

        StreamingTransport transport(hal);
        CollectorSink sink;

        auto r = iface(transport).transact(
            [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
        REQUIRE(r.has_value());
        REQUIRE(sink.get("status") == "ok");
        REQUIRE(hal.rx.empty());
    }
}


// ---------------------------------------------------------------------------
// Frame boundary: HAL returns 0 before data (defensive retry in frame_read)
// ---------------------------------------------------------------------------

TEST_CASE("transact: frame_read retries when HAL returns 0") {
    // ZeroThenDataHal returns 0 for the first N reads, then delivers data.
    // This exercises the defensive retry loop in frame_read for HALs that
    // return 0 instead of blocking or returning an error.
    struct ZeroThenDataHal : MockHal {
        int zeros_remaining;
        uint32_t time = 0;

        explicit ZeroThenDataHal(int zeros) : zeros_remaining(zeros) {}

        Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
            if (zeros_remaining > 0) {
                --zeros_remaining;
                return size_t(0);
            }
            return MockHal::read(buf, max_len, timeout_ms);
        }

        void delay(uint32_t ms) override { time += ms; }
        uint32_t millis() override { return time; }
    };

    ZeroThenDataHal hal(3);  // 3 zero-returns before data arrives
    hal.queue_response(R"({"v":"ok"})");

    StreamingTransport transport(hal);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.get("v") == "ok");
}


// ---------------------------------------------------------------------------
// Wire debug: receive event captures response JSON
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// .into(T&) works via streaming SAX path
// ---------------------------------------------------------------------------

namespace { struct SensorReading { float temperature; int32_t humidity; NOTE_FIELDS(temperature, humidity) }; }

TEST_CASE("into<T>() parses body from streaming SAX path") {
    MockHal hal;
    // Simulate note.get response with a body object
    hal.queue_response(R"({"payload":"dGVzdA==","time":1234,"body":{"temperature":23.5,"humidity":65}})");

    StreamingTransport transport(hal);
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

    CHECK(reading.temperature == Approx(23.5f));
    CHECK(reading.humidity == 65);
}


// ---------------------------------------------------------------------------
// Wire debug: receive event captures response JSON
// ---------------------------------------------------------------------------

TEST_CASE("transact: wire debug captures response JSON") {
    MockHal hal;
    hal.queue_response(R"({"status":"ok","count":7})");

    StreamingTransport transport(hal);

    std::string captured_recv;
    DebugListener debug{};
    debug.ctx = &captured_recv;
    debug.on_wire = [](const WireEvent& ev, void* ctx) {
        if (ev.direction == WireDirection::Receive)
            *static_cast<std::string*>(ctx) = std::string(ev.json.data(), ev.json.size());
    };
    transport.set_debug(debug);

    CollectorSink sink;
    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    // Wire recv should contain the full response JSON (minus \n frame delimiter)
    REQUIRE_FALSE(captured_recv.empty());
    REQUIRE(captured_recv.find("\"status\":\"ok\"") != std::string::npos);
    REQUIRE(captured_recv.find("\"count\":7") != std::string::npos);
}


// ---------------------------------------------------------------------------
// reset: clears initialized state
// ---------------------------------------------------------------------------

TEST_CASE("reset: forces re-initialization on next transact") {
    struct CountingHal : MockHal {
        int reset_count = 0;
        bool reset() override {
            ++reset_count;
            return true;
        }
    };

    CountingHal hal;
    hal.queue_response("{\"ok\":true}");

    StreamingTransport transport(hal, 1, 0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    int count_after_first = hal.reset_count;

    // Reset transport — next transact should call hal.reset() again
    transport.reset();

    hal.queue_response("{\"ok\":true}");
    CollectorSink sink2;
    auto r2 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink2, 5000);
    REQUIRE(r2.has_value());
    REQUIRE(hal.reset_count > count_after_first);
}
