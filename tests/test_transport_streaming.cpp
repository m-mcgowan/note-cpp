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
        // Protocol line terminator (streaming parser reads until } then
        // trailing whitespace is ignored, but real Notecard sends \r\n).
        rx.push_back(static_cast<uint8_t>('\r'));
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
// transact: sink receives reset on retry
// ---------------------------------------------------------------------------

TEST_CASE("transact: sink is reset between retries") {
    MockHal hal;
    // No data queued — both attempts fail. Sink should be reset on each retry.
    StreamingTransport transport(hal, /*max_retries=*/2, /*retry_delay_ms=*/0);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE_FALSE(r.has_value());
    // Sink should have been reset for each retry (attempts 1 and 2)
    REQUIRE(sink.reset_count == 2);
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
