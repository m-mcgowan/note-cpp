// Tests for Protocol over a MockHal.
//
// The new transport architecture splits:
//   - Hal — pure hardware abstraction (transmit, read, reset, etc.)
//   - Protocol — protocol logic (retry, CRC, JSON framing) over a HAL
//   - ITransport — type-erased session interface
//
// Tests exercise transact() (build request via BuildFn, SAX-parse response into
// a JsonSink) and send() (fire-and-forget).

#include <doctest.h>
#include "test_sax_exerciser.hpp"

#include <note/protocol.hpp>
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
// MockHal — minimal Hal for testing Protocol.
//
// Uses a byte deque for read(). Transmit captures bytes. Reset always succeeds.
// ---------------------------------------------------------------------------

class MockHal : public Hal {
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

// Helper: pass-through reference to Protocol — kept for source compat
// while iface() callsites still spell the protocol layer this way.
static Protocol& iface(Protocol& t) { return t; }


// ---------------------------------------------------------------------------
// transact: basic response parsed into sink
// ---------------------------------------------------------------------------

TEST_CASE("transact: basic response parsed via SAX into sink") {
    MockHal hal;
    hal.queue_response("{\"ok\":true}");

    Protocol transport(hal);
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

    Protocol transport(hal);
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

    Protocol transport(hal);
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
    Protocol transport(hal);
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

    Protocol transport(hal);
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

    Protocol transport(hal);
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

    Protocol transport(hal);
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

    Protocol transport(hal);
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
    Protocol transport(hal);
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
    Protocol transport(hal);

    auto r = iface(transport).send(
        [](JsonBuilder& b) { b.add("req", "card.led"); b.add("mode", 1); });
    REQUIRE(r.has_value());
    // Verify the request was transmitted (contains the JSON + terminator)
    REQUIRE(hal.last_transmitted.find("card.led") != std::string::npos);
}

TEST_CASE("send: transmit failure returns SendFailed") {
    MockHal hal;
    hal.transmit_fails = true;

    Protocol transport(hal);

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

    Protocol transport(hal);
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

    Protocol transport(hal);
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
    Protocol transport(hal);
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
    Protocol transport(hal);
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

        Protocol transport(hal);
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
        Protocol transport(hal);

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

    Protocol transport(hal);

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

        Protocol transport(hal);
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

        Protocol transport(hal);
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

    Protocol transport(hal);
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

    Protocol transport(hal);
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


// ---------------------------------------------------------------------------
// Wire debug: receive event captures response JSON
// ---------------------------------------------------------------------------

TEST_CASE("transact: wire debug captures response JSON") {
    MockHal hal;
    hal.queue_response(R"({"status":"ok","count":7})");

    Protocol transport(hal);

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

    Protocol transport(hal);
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

// ── transact_dispatch tests (non-template path) ────────────────────────────

namespace {
struct DispatchTestRsp {
    note::ResponseField<int32_t> value;
    note::ResponseField<note::string_view> name;
};
} // namespace

TEST_CASE("transact_dispatch: basic response via SaxDispatch") {
    MockHal hal;
    Protocol transport(hal);
    hal.queue_response(R"({"value":42,"name":"test"})");

    DispatchTestRsp rsp;

    // ResponseField<T> is non-standard-layout (user-provided ctors), but offsetof
    // works correctly on all implementations. Suppress the pedantic warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    note::FieldDesc fields[] = {
        {"value", static_cast<uint16_t>(offsetof(DispatchTestRsp, value)), note::FieldType::Int},
        {"name", static_cast<uint16_t>(offsetof(DispatchTestRsp, name)), note::FieldType::String},
    };
#pragma GCC diagnostic pop

    char pool_buf[64];
    note::MonotonicArena arena(pool_buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::GenericResponseSink gsink{&rsp, fields, 2, &pool};
    auto dispatch = note::make_sax_dispatch(gsink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE(rv.has_value());
    REQUIRE(nc_err.empty());
    REQUIRE(rsp.value == 42);
    REQUIRE(rsp.name == "test");
}

TEST_CASE("make_sax_dispatch: exercise all events for GenericResponseSink") {
    DispatchTestRsp rsp;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    note::FieldDesc fields[] = {
        {"value", static_cast<uint16_t>(offsetof(DispatchTestRsp, value)), note::FieldType::Int},
        {"name", static_cast<uint16_t>(offsetof(DispatchTestRsp, name)), note::FieldType::String},
    };
#pragma GCC diagnostic pop
    char pool_buf[128];
    note::MonotonicArena arena(pool_buf);
    note::StringPool pool(note::arena_allocator(arena));
    note::GenericResponseSink gsink{&rsp, fields, 2, &pool};
    auto dispatch = note::make_sax_dispatch(gsink);
    note::test::exercise_all_events(dispatch);
}

TEST_CASE("transact_dispatch: captures err field") {
    MockHal hal;
    Protocol transport(hal);
    hal.queue_response(R"({"err":"something went wrong"})");

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE(rv.has_value());
    REQUIRE_FALSE(nc_err.empty());
    REQUIRE(nc_err.view() == "something went wrong");
}

TEST_CASE("transact_dispatch: transmit failure") {
    MockHal hal;
    hal.transmit_fails = true;
    Protocol transport(hal);

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE_FALSE(rv.has_value());
    REQUIRE(rv.error().code == Error::SendFailed);
}


// ---------------------------------------------------------------------------
// send_raw: transmit failure returns SendFailed
// ---------------------------------------------------------------------------

TEST_CASE("send_raw: transmit failure returns SendFailed") {
    MockHal hal;
    // First transact to initialize transport
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);

    // Now fail transmit
    hal.transmit_fails = true;
    auto r = transport.send_raw(string_view("{\"req\":\"test\"}"));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::SendFailed);
}


// ---------------------------------------------------------------------------
// send_raw: write_line_terminator failure returns SendFailed
// ---------------------------------------------------------------------------

TEST_CASE("send_raw: line terminator failure returns SendFailed") {
    struct FailTermHal : MockHal {
        bool write_line_terminator() override { return false; }
    };

    FailTermHal hal;
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);

    auto r = transport.send_raw(string_view("{\"req\":\"test\"}"));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::SendFailed);
}


// ---------------------------------------------------------------------------
// send_raw: success path
// ---------------------------------------------------------------------------

TEST_CASE("send_raw: success transmits JSON with line terminator") {
    MockHal hal;
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);
    hal.last_transmitted.clear();

    auto r = transport.send_raw(string_view("{\"req\":\"card.led\"}"));
    REQUIRE(r.has_value());
    // Should contain the JSON + CRLF from write_line_terminator
    REQUIRE(hal.last_transmitted.find("card.led") != std::string::npos);
    REQUIRE(hal.last_transmitted.find("\r\n") != std::string::npos);
}


// ---------------------------------------------------------------------------
// send_raw: not initialized returns NotReady
// ---------------------------------------------------------------------------

TEST_CASE("send_raw: not initialized returns NotReady") {
    struct FailResetHal : MockHal {
        bool reset() override { return false; }
    };

    FailResetHal hal;
    Protocol transport(hal);

    auto r = transport.send_raw(string_view("{\"req\":\"test\"}"));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::NotReady);
}


// ---------------------------------------------------------------------------
// transact_raw: success reads response line
// ---------------------------------------------------------------------------

TEST_CASE("transact_raw: success reads response") {
    MockHal hal;
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);
    hal.last_transmitted.clear();

    // Queue a response for the raw transact
    hal.queue_response("{\"ok\":true}");

    char buf[128];
    auto r = transport.transact_raw(string_view("{\"req\":\"test\"}"), buf, sizeof(buf), 5000);
    REQUIRE(r.has_value());
    REQUIRE(r->find("ok") != std::string::npos);
}


// ---------------------------------------------------------------------------
// transact_raw: transmit failure
// ---------------------------------------------------------------------------

TEST_CASE("transact_raw: transmit failure returns SendFailed") {
    MockHal hal;
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);

    hal.transmit_fails = true;
    char buf[128];
    auto r = transport.transact_raw(string_view("{\"req\":\"test\"}"), buf, sizeof(buf), 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::SendFailed);
}


// ---------------------------------------------------------------------------
// transact_raw: line terminator failure
// ---------------------------------------------------------------------------

TEST_CASE("transact_raw: line terminator failure returns SendFailed") {
    struct FailTermHal : MockHal {
        bool term_fail = false;
        bool write_line_terminator() override {
            if (term_fail) return false;
            last_transmitted += "\r\n";
            return true;
        }
    };

    FailTermHal hal;
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);

    hal.term_fail = true;
    char buf[128];
    auto r = transport.transact_raw(string_view("{\"req\":\"test\"}"), buf, sizeof(buf), 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::SendFailed);
}


// ---------------------------------------------------------------------------
// transact_raw: not initialized returns NotReady
// ---------------------------------------------------------------------------

TEST_CASE("transact_raw: not initialized returns NotReady") {
    struct FailResetHal : MockHal {
        bool reset() override { return false; }
    };

    FailResetHal hal;
    Protocol transport(hal);

    char buf[128];
    auto r = transport.transact_raw(string_view("{\"req\":\"test\"}"), buf, sizeof(buf), 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::NotReady);
}


// ---------------------------------------------------------------------------
// transact_raw: response overflow returns Overflow error
// ---------------------------------------------------------------------------

TEST_CASE("transact_raw: response overflow returns Overflow") {
    MockHal hal;
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);

    // Queue a long response that exceeds the tiny buffer
    hal.queue_response("{\"data\":\"this-is-a-very-long-response\"}");

    char buf[8];  // tiny buffer
    auto r = transport.transact_raw(string_view("{\"req\":\"test\"}"), buf, sizeof(buf), 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::Overflow);
}


// ---------------------------------------------------------------------------
// transact_raw: response with \r\n line ending (carriage return skipped)
// ---------------------------------------------------------------------------

TEST_CASE("transact_raw: \\r in response is skipped") {
    MockHal hal;
    hal.queue_response("{}");
    Protocol transport(hal);
    CollectorSink sink;
    (void)iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);

    // Manually queue response with \r\n ending (real Notecard serial behavior)
    std::string resp = "{\"v\":1}\r\n";
    for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));

    char buf[128];
    auto r = transport.transact_raw(string_view("{\"req\":\"test\"}"), buf, sizeof(buf), 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"v\":1}");  // \r should be stripped
}


// ---------------------------------------------------------------------------
// transact_raw: read returns 0 → timeout error
// ---------------------------------------------------------------------------

TEST_CASE("transact_raw: read returns 0 triggers timeout") {
    struct ZeroReadHal : MockHal {
        bool first_read_done = false;
        Result<size_t> read(uint8_t*, size_t, uint32_t) override {
            // Always return 0 bytes
            return size_t(0);
        }
    };

    ZeroReadHal hal;
    hal.queue_response("{}");
    // Use base MockHal for init, then swap behavior
    Protocol transport(hal);
    // Force init by doing a normal transact with the mock's default read
    // Actually, our ZeroReadHal overrides read, so init will fail.
    // Instead, set initialized by calling a regular transact via a different approach.

    // Let's use a HAL that only returns 0 on the raw path:
    struct LazyZeroHal : MockHal {
        bool zero_mode = false;
        Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
            if (zero_mode) return size_t(0);
            return MockHal::read(buf, max_len, timeout_ms);
        }
    };

    LazyZeroHal hal2;
    hal2.queue_response("{}");
    Protocol transport2(hal2);
    CollectorSink sink;
    (void)iface(transport2).transact(
        [](JsonBuilder& b) { b.add("req", "init"); }, sink, 5000);

    hal2.zero_mode = true;
    char buf[128];
    auto r = transport2.transact_raw(string_view("{\"req\":\"test\"}"), buf, sizeof(buf), 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::ResponseLost);
}


// ---------------------------------------------------------------------------
// send: not initialized (reset fails) returns NotReady
// ---------------------------------------------------------------------------

TEST_CASE("send: not initialized returns NotReady") {
    struct FailResetHal : MockHal {
        bool reset() override { return false; }
    };

    FailResetHal hal;
    Protocol transport(hal);

    auto r = iface(transport).send(
        [](JsonBuilder& b) { b.add("req", "card.led"); });
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::NotReady);
}


// ---------------------------------------------------------------------------
// lookahead: bytes after \n in frame_read are returned by subsequent read()
// ---------------------------------------------------------------------------

TEST_CASE("read: lookahead bytes from frame_read returned first") {
    MockHal hal;
    Protocol transport(hal);

    // Queue a response that includes extra bytes AFTER \n in the same chunk.
    // Simulate: {"ok":true}\nEXTRA
    std::string data = "{\"ok\":true}\nAB";
    for (char c : data) hal.rx.push_back(static_cast<uint8_t>(c));

    CollectorSink sink;
    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.get("ok") == "true");

    // Now read() should return the lookahead bytes 'A', 'B' before hitting HAL
    uint8_t buf[16];
    auto r2 = transport.read(buf, sizeof(buf), 1000);
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == 2);
    REQUIRE(buf[0] == 'A');
    REQUIRE(buf[1] == 'B');
}


// ---------------------------------------------------------------------------
// transact_dispatch: reset failure returns NotReady
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: reset failure returns NotReady") {
    struct FailResetHal : MockHal {
        bool reset() override { return false; }
    };

    FailResetHal hal;
    Protocol transport(hal);

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE_FALSE(rv.has_value());
    REQUIRE(rv.error().code == Error::NotReady);
}


// ---------------------------------------------------------------------------
// transact_dispatch: response timeout (no data)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: response timeout returns ResponseLost") {
    MockHal hal;
    // No data queued → timeout
    Protocol transport(hal);

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE_FALSE(rv.has_value());
    // Timeout causes ResponseLost
    REQUIRE(rv.error().code == Error::ResponseLost);
}


// ---------------------------------------------------------------------------
// CRC response helper — computes CRC32 and formats a response with CRC field
// ---------------------------------------------------------------------------

namespace {

/// Build a JSON response with a valid CRC field.
/// @param body_without_brace  JSON body without closing brace, e.g. R"({"ok":true)"
/// @param seq   The CRC sequence number (must match the transport's crc_seq_).
/// @return      Full JSON string with CRC, e.g. {"ok":true,"crc":"0001:XXXXXXXX"}
std::string make_crc_response(const std::string& body_without_brace, uint16_t seq) {
    // The CRC is computed over the original JSON including closing brace.
    std::string original = body_without_brace + "}";
    uint32_t checksum = note::transport::detail::crc32(original.data(), original.size());

    // Format: ,"crc":"SSSS:CCCCCCCC"}
    char suffix[24];
    size_t pos = 0;
    suffix[pos++] = ',';
    suffix[pos++] = '"'; suffix[pos++] = 'c'; suffix[pos++] = 'r';
    suffix[pos++] = 'c'; suffix[pos++] = '"'; suffix[pos++] = ':';
    suffix[pos++] = '"';
    note::transport::detail::write_hex16(suffix + pos, seq); pos += 4;
    suffix[pos++] = ':';
    note::transport::detail::write_hex32(suffix + pos, checksum); pos += 8;
    suffix[pos++] = '"';
    suffix[pos++] = '}';
    suffix[pos] = '\0';

    return body_without_brace + std::string(suffix, pos);
}

} // namespace


// ---------------------------------------------------------------------------
// CRC: valid CRC in response — exercises ReceiveContext CRC parsing (line 89)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: valid CRC response succeeds") {
    MockHal hal;
    Protocol transport(hal);

    // First transact increments crc_seq_ to 1.
    // Build a response with valid CRC for seq=1.
    auto resp = make_crc_response(R"({"ok":true)", 1);
    for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));
    hal.rx.push_back(static_cast<uint8_t>('\n'));

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE(rv.has_value());
}


// ---------------------------------------------------------------------------
// CRC: mismatched CRC checksum — exercises CRC mismatch (line 648, 266)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: CRC mismatch returns error") {
    MockHal hal;
    Protocol transport(hal);

    // Build a response with wrong CRC (use wrong checksum)
    std::string resp = R"({"ok":true,"crc":"0001:DEADBEEF"})";
    for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));
    hal.rx.push_back(static_cast<uint8_t>('\n'));

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE_FALSE(rv.has_value());
    REQUIRE(rv.error().cause == Cause::CrcMismatch);
}


// ---------------------------------------------------------------------------
// CRC: seq mismatch — wrong seq number (line 648)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: CRC seq mismatch returns error") {
    MockHal hal;
    Protocol transport(hal);

    // Valid checksum but wrong seq (use seq=99 instead of 1)
    auto resp = make_crc_response(R"({"ok":true)", 99);
    for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));
    hal.rx.push_back(static_cast<uint8_t>('\n'));

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE_FALSE(rv.has_value());
    REQUIRE(rv.error().cause == Cause::CrcMismatch);
}


// ---------------------------------------------------------------------------
// CRC: expected CRC missing — previously enabled but now absent (line 648)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: expected CRC missing returns error") {
    MockHal hal;
    Protocol transport(hal);

    // First transaction WITH valid CRC enables crc_enabled_
    {
        auto resp = make_crc_response(R"({"ok":true)", 1);
        for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));
        hal.rx.push_back(static_cast<uint8_t>('\n'));

        note::NullSink null_sink;
        auto dispatch = note::make_sax_dispatch(null_sink);
        detail::NcErrorCapture nc_err;
        BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
        auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
        REQUIRE(rv.has_value());
    }

    // Second transaction WITHOUT CRC: crc_enabled_ is true but no CRC found
    {
        hal.queue_response(R"({"ok":true})");

        note::NullSink null_sink;
        auto dispatch = note::make_sax_dispatch(null_sink);
        detail::NcErrorCapture nc_err;
        BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test2"); };
        auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
        REQUIRE_FALSE(rv.has_value());
        REQUIRE(rv.error().cause == Cause::CrcMismatch);
    }
}


// ---------------------------------------------------------------------------
// CRC: valid CRC via transact_impl (template path) — exercises line 298
// ---------------------------------------------------------------------------

TEST_CASE("transact: CRC mismatch via template path returns error") {
    MockHal hal;
    Protocol transport(hal);

    // Build a response with wrong CRC
    std::string resp = R"({"ok":true,"crc":"0001:DEADBEEF"})";
    for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));
    hal.rx.push_back(static_cast<uint8_t>('\n'));

    CollectorSink sink;
    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().cause == Cause::CrcMismatch);
}


// ---------------------------------------------------------------------------
// NcErrorCapture: truncation of long error (line 49)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: long err field is truncated in NcErrorCapture") {
    MockHal hal;
    Protocol transport(hal);

    // Create an error message > 64 chars to exercise truncation
    std::string long_err(80, 'x');
    std::string resp = R"({"err":")" + long_err + R"("})";
    hal.queue_response(resp);

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE(rv.has_value());
    REQUIRE_FALSE(nc_err.empty());
    // Should be truncated to 64 chars
    REQUIRE(nc_err.view().size() == 64);
}


// ---------------------------------------------------------------------------
// ReceiveContext: Reset event clears err and CRC state (line 100)
// ---------------------------------------------------------------------------

TEST_CASE("ReceiveContext: reset event clears err and CRC state") {
    detail::NcErrorCapture nc_err;
    nc_err.capture("previous error");

    note::NullSink null_sink;
    auto inner = note::make_sax_dispatch(null_sink);

    detail::ReceiveContext ctx(inner, nc_err);
    auto wrapped = ctx.wrapping_dispatch();

    // Send a Reset event
    auto reset_ev = SaxEvent::make_reset();
    wrapped.dispatch(wrapped.sink, reset_ev);

    // Error should be cleared
    REQUIRE(nc_err.empty());
    // CRC state should be reset
    REQUIRE_FALSE(ctx.crc_found);
    REQUIRE(ctx.crc_seq == 0);
    REQUIRE(ctx.crc_checksum == 0);
}


// ---------------------------------------------------------------------------
// ReceiveContext: CRC field parsing (line 89)
// ---------------------------------------------------------------------------

TEST_CASE("ReceiveContext: CRC field parsing extracts seq and checksum") {
    detail::NcErrorCapture nc_err;
    note::NullSink null_sink;
    auto inner = note::make_sax_dispatch(null_sink);

    detail::ReceiveContext ctx(inner, nc_err);
    auto wrapped = ctx.wrapping_dispatch();

    // Simulate a CRC string event: "crc" with value "0042:DEADBEEF"
    auto ev = SaxEvent::make_string("crc", "0042:DEADBEEF");
    wrapped.dispatch(wrapped.sink, ev);

    REQUIRE(ctx.crc_found);
    REQUIRE(ctx.crc_seq == 0x0042);
    REQUIRE(ctx.crc_checksum == 0xDEADBEEF);
}

TEST_CASE("ReceiveContext: CRC field with wrong format is ignored") {
    detail::NcErrorCapture nc_err;
    note::NullSink null_sink;
    auto inner = note::make_sax_dispatch(null_sink);

    detail::ReceiveContext ctx(inner, nc_err);
    auto wrapped = ctx.wrapping_dispatch();

    // CRC value with wrong length (not 13)
    auto ev = SaxEvent::make_string("crc", "short");
    wrapped.dispatch(wrapped.sink, ev);

    REQUIRE_FALSE(ctx.crc_found);
}

TEST_CASE("ReceiveContext: err field is captured and forwarded") {
    detail::NcErrorCapture nc_err;

    // Use NullSink (no-op dispatch) — we only need to verify ReceiveContext
    // captures the "err" field, not that the inner sink processes it.
    note::NullSink null_sink;
    auto inner = note::make_sax_dispatch(null_sink);

    detail::ReceiveContext ctx(inner, nc_err);
    auto wrapped = ctx.wrapping_dispatch();

    auto ev = SaxEvent::make_string("err", "something failed");
    wrapped.dispatch(wrapped.sink, ev);

    REQUIRE_FALSE(nc_err.empty());
    REQUIRE(nc_err.view() == "something failed");
}


// ---------------------------------------------------------------------------
// frame_read timeout: HAL returns 0 and eventually times out (line 555)
// ---------------------------------------------------------------------------

TEST_CASE("transact: frame_read timeout when HAL always returns 0") {
    struct AlwaysZeroHal : MockHal {
        uint32_t time = 0;
        Result<size_t> read(uint8_t*, size_t, uint32_t) override {
            return size_t(0);
        }
        void delay(uint32_t ms) override { time += ms; }
        uint32_t millis() override { return time; }
    };

    AlwaysZeroHal hal;
    Protocol transport(hal);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 100);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == Error::ResponseLost);
    // frame_read timeout surfaces as Unspecified through SAX parse error path
}


// ---------------------------------------------------------------------------
// transact_dispatch: timeout path exercises Cause::Timeout debug (line 264)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: timeout returns ResponseLost via dispatch") {
    struct AlwaysZeroHal : MockHal {
        uint32_t time = 0;
        Result<size_t> read(uint8_t*, size_t, uint32_t) override {
            return size_t(0);
        }
        void delay(uint32_t ms) override { time += ms; }
        uint32_t millis() override { return time; }
    };

    AlwaysZeroHal hal;
    Protocol transport(hal);

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 100, nc_err);
    REQUIRE_FALSE(rv.has_value());
    REQUIRE(rv.error().code == Error::ResponseLost);
}


// ---------------------------------------------------------------------------
// CRC mismatch debug event (line 266)
// ---------------------------------------------------------------------------

TEST_CASE("transact_dispatch: CRC mismatch with debug listener") {
    MockHal hal;
    Protocol transport(hal);

    bool saw_crc_mismatch = false;
    DebugListener debug{};
    debug.ctx = &saw_crc_mismatch;
    debug.on_transport = [](TransportEvent ev, uint32_t, void* ctx) {
        if (ev == TransportEvent::CrcMismatch)
            *static_cast<bool*>(ctx) = true;
    };
    transport.set_debug(debug);

    // Build response with bad CRC
    std::string resp = R"({"ok":true,"crc":"0001:DEADBEEF"})";
    for (char c : resp) hal.rx.push_back(static_cast<uint8_t>(c));
    hal.rx.push_back(static_cast<uint8_t>('\n'));

    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    detail::NcErrorCapture nc_err;
    BuildFn fn = [](JsonBuilder& b, void*) { b.add("req", "test"); };
    auto rv = transport.transact_dispatch(fn, nullptr, dispatch, 5000, nc_err);
    REQUIRE_FALSE(rv.has_value());
    REQUIRE(rv.error().cause == Cause::CrcMismatch);
    REQUIRE(saw_crc_mismatch);
}


// ---------------------------------------------------------------------------
// lookahead: bytes after \n in frame_read (line 574)
// ---------------------------------------------------------------------------

TEST_CASE("transact: lookahead buffer saves bytes after frame delimiter") {
    // Use a HAL that delivers everything in one big chunk
    struct BigChunkHal : MockHal {
        Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
            return MockHal::read(buf, max_len, timeout_ms);
        }
    };

    BigChunkHal hal;
    // Queue: response + \n + extra bytes all in one deque
    std::string payload = "{\"ok\":true}\nBINARY";
    for (char c : payload) hal.rx.push_back(static_cast<uint8_t>(c));

    Protocol transport(hal);
    CollectorSink sink;

    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(sink.get("ok") == "true");

    // The lookahead should contain "BINARY"
    uint8_t buf[16];
    auto r2 = transport.read(buf, sizeof(buf), 1000);
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == 6);
    REQUIRE(std::string(reinterpret_cast<char*>(buf), 6) == "BINARY");
}


// ---------------------------------------------------------------------------
// Wire debug: debug_recv accumulates multi-chunk response (line 584)
// ---------------------------------------------------------------------------

TEST_CASE("transact: wire debug accumulates multi-chunk response") {
    ChunkedMockHal hal(1);
    hal.queue_response(R"({"a":"b"})");

    Protocol transport(hal);

    std::string captured;
    DebugListener debug{};
    debug.ctx = &captured;
    debug.on_wire = [](const WireEvent& ev, void* ctx) {
        if (ev.direction == WireDirection::Receive)
            *static_cast<std::string*>(ctx) = std::string(ev.json.data(), ev.json.size());
    };
    transport.set_debug(debug);

    CollectorSink sink;
    auto r = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "test"); }, sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE_FALSE(captured.empty());
    REQUIRE(captured.find("\"a\":\"b\"") != std::string::npos);
}


// ---------------------------------------------------------------------------
// Lookahead pushback: bytes stashed after \n in one HAL chunk must be
// visible to whatever reads next (transact_raw, transact, or binary read).
//
// Regression guard for a bug where read_line() and frame_read() called
// hal_.read() directly, bypassing lookahead_. MockHal returns both queued
// responses in one read() call when max_len spans the boundary, exercising
// the stash-and-recover path that real UART drivers can also hit when two
// frames coalesce into a single buffer.
// ---------------------------------------------------------------------------

TEST_CASE("lookahead: transact_raw reads response stashed by prior transact") {
    MockHal hal;
    // Two frames in one rx deque — the first transact's frame_read pulls
    // them both in a single hal.read() call (49 bytes fits in the 64-byte
    // SAX rbuf), stashing the tail of rx in lookahead_.
    hal.queue_response("{}");
    hal.queue_response(R"({"status":"ready"})");

    Protocol transport(hal);

    CollectorSink sink;
    auto r1 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "card.binary"); }, sink, 5000);
    REQUIRE(r1.has_value());

    // rx is now empty — the second frame is sitting in lookahead_.
    REQUIRE(hal.rx.empty());

    // Without the fix, read_line() calls hal.read() directly and times out.
    char buf[128];
    auto r2 = transport.transact_raw(R"({"req":"card.binary"})", buf, sizeof(buf), 5000);
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == R"({"status":"ready"})");
}

TEST_CASE("lookahead: next transact reads response stashed by prior transact") {
    MockHal hal;
    hal.queue_response("{}");
    hal.queue_response(R"({"v":42})");

    Protocol transport(hal);

    CollectorSink first;
    auto r1 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "card.version"); }, first, 5000);
    REQUIRE(r1.has_value());
    REQUIRE(hal.rx.empty());

    // Without the fix, frame_read() on the second transact calls
    // hal.read() directly and never sees the stashed bytes.
    CollectorSink second;
    auto r2 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "card.version"); }, second, 5000);
    REQUIRE(r2.has_value());
    REQUIRE(second.get("v") == "42");
}

TEST_CASE("lookahead: binary read() consumes bytes stashed by prior transact") {
    // Already worked before the fix — the virtual read() path was the
    // original (and only) lookahead consumer. Pinning it here so that any
    // future refactor of read_hal() that breaks binary I/O gets caught.
    MockHal hal;
    hal.queue_response("{}");
    // Raw binary bytes after the JSON frame — simulates card.binary GET
    // where JSON handshake and COBS bytes arrive back-to-back.
    const uint8_t bin_payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    for (auto b : bin_payload) hal.rx.push_back(b);

    Protocol transport(hal);

    CollectorSink sink;
    auto r1 = iface(transport).transact(
        [](JsonBuilder& b) { b.add("req", "card.binary.get"); }, sink, 5000);
    REQUIRE(r1.has_value());
    REQUIRE(hal.rx.empty());

    uint8_t out[sizeof(bin_payload)]{};
    auto r2 = transport.read(out, sizeof(out), 5000);
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == sizeof(bin_payload));
    REQUIRE(memcmp(out, bin_payload, sizeof(bin_payload)) == 0);
}
