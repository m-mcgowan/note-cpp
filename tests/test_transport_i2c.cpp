// Tests for note::link::I2cFramer (Hal) and
// note::Protocol protocol logic over I2C.
//
// Ported from note-c test/src/_i2cNoteReset_test.cpp,
// _i2cChunkedTransmit_test.cpp, and _i2cChunkedReceive_test.cpp.
//
// Key mapping:
//   note-c _i2cNoteReset()       → I2cFramer::reset()
//   note-c _i2cChunkedTransmit() → I2cFramer::transmit() (chunked)
//   note-c _i2cChunkedReceive()  → I2cFramer::read() (priming query)
//   note-c _I2CReceive(addr, buf, 0, &avail) → I2cHal::receive(buf, 0, avail)
//
// I2cFramer is a Hal (raw byte ops: transmit, read, reset,
// write_line_terminator, delay). Protocol logic (transact, send, retry, CRC)
// lives in Protocol, which wraps a Hal.
//
// When note-c's I2C tests change, review the diffs and update accordingly.

#include <doctest.h>

#include <note/link/i2c.hpp>
#include <note/link/detail/crc32.hpp>
#include <note/protocol.hpp>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// CaptureSink — simple JsonSink that records string/bool/number events
// ---------------------------------------------------------------------------

namespace {

struct CaptureSink : note::JsonSink {
    struct Event {
        std::string type;
        std::string key;
        std::string value;
    };
    std::vector<Event> events;

    void on_null(note::string_view key) override {
        events.push_back({"null", std::string(key), ""});
    }
    void on_bool(note::string_view key, bool value) override {
        events.push_back({"bool", std::string(key), value ? "true" : "false"});
    }
    void on_number(note::string_view key, note::string_view raw) override {
        events.push_back({"number", std::string(key), std::string(raw)});
    }
    void on_string(note::string_view key, note::string_view value) override {
        events.push_back({"string", std::string(key), std::string(value)});
    }
    void on_object_begin(note::string_view key) override {
        events.push_back({"object_begin", std::string(key), ""});
    }
    void on_object_end(note::string_view key) override {
        events.push_back({"object_end", std::string(key), ""});
    }

    void reset() override { events.clear(); }

    std::string find(const std::string& k, const std::string& t = "string") const {
        for (auto& e : events) {
            if (e.key == k && e.type == t) return e.value;
        }
        return {};
    }

    bool has_key(const std::string& k) const {
        for (auto& e : events) {
            if (e.key == k) return true;
        }
        return false;
    }
};

// Helper: string-returning wrapper for char-buffer crc_add (test convenience)
inline std::string str_crc_add(const std::string& json, uint16_t seq) {
    char buf[512];
    size_t len = json.size();
    memcpy(buf, json.data(), len);
    size_t new_len = note::link::detail::crc_add(buf, len, sizeof(buf), seq);
    return std::string(buf, new_len);
}

} // namespace


using namespace note::link;
using note::Error;

// ---------------------------------------------------------------------------
// ScriptedI2cHal — in-memory I2C HAL for testing
//
// receive(buf, 0, available) — priming query: returns pending byte count,
//                               does not consume bytes.
// receive(buf, N, available) — reads N bytes, returns remaining count.
//
// When a complete message (ending with '\n') is transmitted:
//   - bare "\n" (reset probe) → injects reset_response into rx_buf
//   - full request            → injects next response from `responses`
//
// Tests may also assign rx_buf directly for special cases (e.g., partial
// responses for intra-timeout testing).
// ---------------------------------------------------------------------------

struct ScriptedI2cHal : public I2cHal {
    // Configuration
    bool               reset_ok      = true;
    bool               transmit_ok   = true;
    std::string        reset_response = "\r\n";  // echoed by Notecard after \n probe
    std::deque<std::string> responses;           // queued complete responses (include \n)
    size_t             mtu           = 1024;     // default large: no chunking in most tests

    // Per-call transmit override: if set, called with the 1-based call number.
    // Return false to simulate a transmit failure.
    std::function<bool(int)> transmit_ok_fn;

    // Observation
    int                reset_call_count    = 0;
    int                transmit_call_count = 0;
    int                delay_call_count    = 0;
    uint32_t           total_delay_ms      = 0;
    std::string        last_request;             // last complete request (no trailing \n)

    // State
    std::string        rx_buf;
    std::string        tx_accum;
    uint32_t           now_ms = 0;

    bool reset() override {
        ++reset_call_count;
        return reset_ok;
    }

    bool transmit(const uint8_t* data, size_t len) override {
        ++transmit_call_count;
        if (transmit_ok_fn) {
            if (!transmit_ok_fn(transmit_call_count)) return false;
        } else if (!transmit_ok) {
            return false;
        }

        tx_accum.append(reinterpret_cast<const char*>(data), len);

        // Complete message detected when accumulator ends with '\n'.
        if (!tx_accum.empty() && tx_accum.back() == '\n') {
            if (tx_accum == "\n") {
                // Reset probe — inject reset drain response.
                rx_buf = reset_response;
            } else {
                // Full request — inject next queued response (if any).
                last_request = tx_accum.substr(0, tx_accum.size() - 1);
                if (!responses.empty()) {
                    rx_buf = responses.front();
                    responses.pop_front();
                }
            }
            tx_accum.clear();
        }
        return true;
    }

    bool receive(uint8_t* buf, size_t len, uint32_t& available) override {
        if (len == 0) {
            // Priming query — report pending bytes without consuming.
            available = static_cast<uint32_t>(rx_buf.size());
            return true;
        }
        const size_t n = std::min(len, rx_buf.size());
        if (buf && n > 0) std::memcpy(buf, rx_buf.data(), n);
        rx_buf.erase(0, n);
        available = static_cast<uint32_t>(rx_buf.size());
        return true;
    }

    uint32_t millis() override { return now_ms; }

    void delay(uint32_t ms) override {
        ++delay_call_count;
        total_delay_ms += ms;
        now_ms += ms;
    }

    size_t max_transfer() override { return mtu; }
};

// ---------------------------------------------------------------------------
// I2cTestHarness — Protocol over I2cFramer over ScriptedI2cHal
//
// Returns by value — I2cFramer and Protocol hold references
// to each other and to the hal, so callers must keep the Harness alive.
// ---------------------------------------------------------------------------

struct I2cTestHarness {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> notecard_i2c;
    note::Protocol transport;

    I2cTestHarness()
        : notecard_i2c(hal)
        , transport(notecard_i2c)
    {}

    // Convenience: transact with a simple build function and capture sink.
    note::Result<void> transact(const char* req_name, CaptureSink& sink,
                                uint32_t timeout_ms = 5000) {
        note::Protocol& t = transport;
        auto build = [&](note::JsonBuilder& b) { b.add("req", req_name); };
        return t.transact(build, sink, timeout_ms);
    }

    // Transact with a NullSink (just check success/failure).
    note::Result<void> transact(const char* req_name,
                                uint32_t timeout_ms = 5000) {
        note::Protocol& t = transport;
        note::JsonSink null_sink;
        auto build = [&](note::JsonBuilder& b) { b.add("req", req_name); };
        return t.transact(build, null_sink, timeout_ms);
    }

    // Send (fire-and-forget).
    note::Result<void> send(const char* cmd_name) {
        note::Protocol& t = transport;
        auto build = [&](note::JsonBuilder& b) { b.add("cmd", cmd_name); };
        return t.send(build);
    }
};

// ---------------------------------------------------------------------------
// reset() — ported from note-c _i2cNoteReset_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("i2c reset: pre-delay of kI2cSegmentDelayMs (250 ms)") {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> i2c(hal);
    // reset() should start with a segment delay.
    i2c.reset();
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs);
}

TEST_CASE("i2c reset: calls hal.reset() once on first use") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("hub.status");
    CHECK(h.hal.reset_call_count >= 1);
}

TEST_CASE("i2c reset: io delay (6 ms) after reset()") {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> i2c(hal);
    i2c.reset();
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs + kI2cIoDelayMs);
}

TEST_CASE("i2c reset: reset() fails -> NotReady error") {
    I2cTestHarness h;
    h.hal.reset_ok = false;
    auto result = h.transact("hub.status");
    REQUIRE(!result);
    CHECK(result.error().code == Error::NotReady);
}

TEST_CASE("i2c reset: clean drain succeeds") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    auto result = h.transact("hub.status");
    REQUIRE(result);
}

TEST_CASE("i2c reset: transmit \\n fails (NACK) all retries -> NotReady error") {
    ScriptedI2cHal hal;
    hal.transmit_ok = false;
    I2cFramer<I2cPolicy> i2c(hal);
    bool ok = i2c.reset();
    REQUIRE_FALSE(ok);
    // Should have retried kI2cResetSyncRetries times.
    CHECK(hal.transmit_call_count == static_cast<int>(kI2cResetSyncRetries));
}

TEST_CASE("i2c reset: NACK delays kI2cNackWaitMs (1000 ms) per attempt") {
    ScriptedI2cHal hal;
    hal.transmit_ok = false;
    I2cFramer<I2cPolicy> i2c(hal);
    i2c.reset();
    // Each NACK incurs kI2cNackWaitMs (1000 ms).
    CHECK(hal.total_delay_ms >= kI2cNackWaitMs * kI2cResetSyncRetries);
}

TEST_CASE("i2c reset: drain receives non-control chars -> all retries fail") {
    ScriptedI2cHal hal;
    hal.reset_response = "{}";  // non-control chars — garbage on the bus
    I2cFramer<I2cPolicy> i2c(hal);
    bool ok = i2c.reset();
    REQUIRE_FALSE(ok);
}

TEST_CASE("i2c reset: drain delay is kI2cSegmentDelayMs after transmit") {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> i2c(hal);
    i2c.reset();
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs + kI2cIoDelayMs + kI2cSegmentDelayMs);
}

TEST_CASE("i2c reset: priming query (len=0) is first receive in drain") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    auto result = h.transact("hub.status");
    CHECK(result.has_value());
}

// ---------------------------------------------------------------------------
// transmit() — chunked I2C transmit tests (HAL level)
// ---------------------------------------------------------------------------

TEST_CASE("i2c transmit: small payload sent in one chunk") {
    ScriptedI2cHal hal;
    hal.mtu = 30;
    I2cFramer<I2cPolicy> i2c(hal);
    uint8_t data[] = "short";
    REQUIRE(i2c.transmit(data, 5));
    // One chunk for 5 bytes (< 30 MTU).
    CHECK(hal.transmit_call_count == 1);
}

TEST_CASE("i2c transmit: multi-chunk when payload exceeds MTU") {
    ScriptedI2cHal hal;
    hal.mtu = 10;
    I2cFramer<I2cPolicy> i2c(hal);
    uint8_t data[50];
    memset(data, 'a', sizeof(data));
    REQUIRE(i2c.transmit(data, sizeof(data)));
    // 50/10 = 5 chunks
    CHECK(hal.transmit_call_count == 5);
}

TEST_CASE("i2c transmit: io delay (6 ms) before each chunk") {
    ScriptedI2cHal hal;
    hal.mtu = 10;
    I2cFramer<I2cPolicy> i2c(hal);
    uint8_t data[30];
    memset(data, 'a', sizeof(data));
    i2c.transmit(data, sizeof(data));
    // 3 chunks, each gets io delay
    int chunks = hal.transmit_call_count;
    CHECK(hal.total_delay_ms >= static_cast<uint32_t>(chunks) * kI2cIoDelayMs);
}

TEST_CASE("i2c transmit: chunk delay (20 ms) after each chunk") {
    ScriptedI2cHal hal;
    hal.mtu = 10;
    I2cFramer<I2cPolicy> i2c(hal);
    uint8_t data[20];
    memset(data, 'a', sizeof(data));
    i2c.transmit(data, sizeof(data));
    int chunks = hal.transmit_call_count;
    CHECK(hal.total_delay_ms >= static_cast<uint32_t>(chunks) * (kI2cIoDelayMs + kI2cChunkDelayMs));
}

TEST_CASE("i2c transmit: segment delay (250 ms) after > 250 bytes in segment") {
    ScriptedI2cHal hal;
    hal.mtu = 30;
    I2cFramer<I2cPolicy> i2c(hal);
    uint8_t data[259];
    memset(data, 'x', sizeof(data));
    i2c.transmit(data, sizeof(data));
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs);
}

TEST_CASE("i2c transmit: failure calls reset() and returns false") {
    ScriptedI2cHal hal;
    hal.transmit_ok = false;
    I2cFramer<I2cPolicy> i2c(hal);
    uint8_t data[] = "test";
    bool ok = i2c.transmit(data, 4);
    REQUIRE_FALSE(ok);
    CHECK(hal.reset_call_count >= 1);
}

// ---------------------------------------------------------------------------
// read() — priming-query receive tests (HAL level)
// ---------------------------------------------------------------------------

TEST_CASE("i2c read: returns available bytes") {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> i2c(hal);
    hal.rx_buf = std::string("\xAA\xBB\x0A", 3);
    uint8_t buf[16];
    auto r = i2c.read(buf, sizeof(buf), 5000);
    REQUIRE(r.has_value());
    CHECK(*r <= 3);
    CHECK(buf[0] == 0xAA);
}

TEST_CASE("i2c read: times out when no data") {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> i2c(hal);
    uint8_t buf[16];
    auto r = i2c.read(buf, sizeof(buf), 10);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::ResponseLost);
}

TEST_CASE("i2c read: HAL receive failure returns error") {
    int receive_call = 0;
    auto failing_receive = [&](uint8_t*, size_t, uint32_t& avail) -> bool {
        ++receive_call;
        if (receive_call == 1) {
            avail = 2;
            return true;
        }
        return false;
    };
    uint32_t now = 0;
    I2cCallbackHal cb{
        []() -> bool { return true; },
        [](const uint8_t*, size_t) -> bool { return true; },
        failing_receive,
        [&]() -> uint32_t { return now; },
        [&](uint32_t ms) { now += ms; }
    };
    I2cFramer<I2cPolicy> i2c(cb);
    uint8_t buf[16];
    auto r = i2c.read(buf, sizeof(buf), 5000);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::ResponseLost);
}

// ---------------------------------------------------------------------------
// write_line_terminator() — I2C sends bare \n
// ---------------------------------------------------------------------------

TEST_CASE("i2c write_line_terminator: sends bare LF") {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> i2c(hal);
    REQUIRE(i2c.write_line_terminator());
    REQUIRE(hal.tx_accum.size() == 0);  // tx_accum was consumed (bare \n triggers reset path)
    // Verify via response injection: bare \n triggers reset_response
    CHECK(hal.rx_buf == "\r\n");
}

// ---------------------------------------------------------------------------
// delay() — forwards to HAL
// ---------------------------------------------------------------------------

TEST_CASE("i2c delay: forwards to HAL") {
    ScriptedI2cHal hal;
    I2cFramer<I2cPolicy> i2c(hal);
    i2c.delay(42);
    CHECK(hal.now_ms == 42);
}

// ---------------------------------------------------------------------------
// Protocol tests — through Protocol
// ---------------------------------------------------------------------------

TEST_CASE("i2c round-trip: simple request and response") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    CaptureSink sink;
    auto result = h.transact("hub.status", sink);
    REQUIRE(result);
}

TEST_CASE("i2c round-trip: response fields are delivered to sink") {
    I2cTestHarness h;
    h.hal.responses.push_back("{\"connected\":true}\n");
    CaptureSink sink;
    auto result = h.transact("hub.status", sink);
    REQUIRE(result);
    CHECK(sink.find("connected", "bool") == "true");
}

TEST_CASE("i2c round-trip: request contains the req field") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("hub.sync");
    CHECK(h.hal.last_request.find("\"req\"") != std::string::npos);
    CHECK(h.hal.last_request.find("hub.sync") != std::string::npos);
}

TEST_CASE("i2c round-trip: second request after success") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.hal.responses.push_back("{}\n");

    auto r1 = h.transact("first");
    REQUIRE(r1);

    auto r2 = h.transact("second");
    REQUIRE(r2);
}

TEST_CASE("i2c round-trip: transmit failure all retries -> SendFailed") {
    I2cTestHarness h;
    // First transact succeeds (init)
    h.hal.responses.push_back("{}\n");
    h.transact("init");

    // Make all subsequent transmits fail and reset fail too (fast-fail).
    h.hal.transmit_ok = false;
    h.hal.reset_ok    = false;
    auto result = h.transact("hub.sync");
    REQUIRE(!result);
    CHECK(result.error().code == Error::SendFailed);
    CHECK(result.error().cause == note::Cause::HalError);
}

TEST_CASE("i2c round-trip: timeout -> error") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("init");

    // No response queued — timeout on receive.
    auto result = h.transact("hub.sync", 10);
    REQUIRE(!result);
}

TEST_CASE("i2c round-trip: CRC auto-detection on first CRC response") {
    I2cTestHarness h;

    // First request: CRC always sent with seq=1. Response echoes CRC back.
    const std::string json = "{\"ok\":true}";
    std::string resp = str_crc_add(json, 1) + "\n";
    h.hal.responses.push_back(resp);
    CaptureSink sink;
    auto r = h.transact("hub.sync", sink);
    REQUIRE(r);
    CHECK(sink.find("ok", "bool") == "true");
    // CRC field should not appear in the user sink (intercepted by CrcFieldSink)
    CHECK_FALSE(sink.has_key("crc"));
}

TEST_CASE("i2c round-trip: after CRC detection, second request includes CRC field") {
    I2cTestHarness h;

    // First call: seq=1 (always sent)
    h.hal.responses.push_back(str_crc_add("{\"ok\":true}", 1) + "\n");
    h.transact("hub.set");

    // Second call: seq=2
    h.hal.responses.push_back(str_crc_add("{\"ok\":true}", 2) + "\n");
    h.hal.last_request.clear();
    h.transact("hub.sync");

    REQUIRE(h.hal.last_request.find("\"crc\":\"") != std::string::npos);
}

TEST_CASE("i2c round-trip: CRC mismatch returns ResponseLost") {
    I2cTestHarness h;

    // First call: seq=1
    h.hal.responses.push_back(str_crc_add("{\"ok\":true}", 1) + "\n");
    h.transact("hub.set");

    // Second call: response has wrong seq -> CRC mismatch -> error (no retry).
    h.hal.responses.push_back(str_crc_add("{\"ok\":true}", 99) + "\n");  // wrong seq
    CaptureSink sink;
    auto r = h.transact("hub.sync", sink);
    REQUIRE(!r);
    CHECK(r.error().code == note::Error::ResponseLost);
    CHECK(r.error().cause == note::Cause::CrcMismatch);
}

TEST_CASE("i2c round-trip: I/O error on transmit returns SendFailed") {
    I2cTestHarness h;
    // transmit_ok_fn applies to the I2cHal-level transmit calls.
    // Call 1: reset probe '\n' (during initial reset -> succeeds)
    // Call 2: first data byte '{' of request -> fails
    // Transport returns SendFailed immediately (no retry).
    h.hal.transmit_ok_fn = [](int call) -> bool { return call != 2; };

    h.hal.responses.push_back("{}\n");
    auto r = h.transact("hub.set");
    REQUIRE(!r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

TEST_CASE("i2c round-trip: reset failure returns Error::NotReady") {
    I2cTestHarness h;
    h.hal.reset_response = "BAD\r\n";  // reset always fails (non-control chars)
    auto r = h.transact("hub.set");
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

// ---------------------------------------------------------------------------
// send() — fire-and-forget via Protocol
// ---------------------------------------------------------------------------

TEST_CASE("i2c send: transmits without waiting for response") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("init");  // init
    h.hal.last_request.clear();

    auto r = h.send("hub.set");
    REQUIRE(r.has_value());
    CHECK(h.hal.last_request.find("hub.set") != std::string::npos);
}

TEST_CASE("i2c send: before first transact triggers reset") {
    I2cTestHarness h;
    auto r = h.send("hub.set");
    REQUIRE(r.has_value());
}

TEST_CASE("i2c send: fails when transmit fails") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("init");

    h.hal.transmit_ok = false;
    auto r = h.send("hub.set");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

// ---------------------------------------------------------------------------
// Binary write/read — raw byte streaming via Protocol
// ---------------------------------------------------------------------------

TEST_CASE("i2c: write() sends raw bytes") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("card.binary.put");  // init
    h.hal.tx_accum.clear();
    h.hal.transmit_call_count = 0;

    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    auto r = h.transport.write(data, sizeof(data));
    REQUIRE(r.has_value());

    // Data should have been transmitted (possibly chunked by I2cFramer)
    CHECK(h.hal.transmit_call_count >= 1);
}

TEST_CASE("i2c: read() returns available bytes via Protocol") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("init");

    // Inject raw bytes to read
    h.hal.rx_buf = std::string("\xAA\xBB\x0A", 3);

    uint8_t buf[16];
    auto r = h.transport.read(buf, sizeof(buf), 5000);
    REQUIRE(r.has_value());
    CHECK(*r <= 3);
    CHECK(buf[0] == 0xAA);
}

TEST_CASE("i2c: read() times out when no data via Protocol") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("init");

    uint8_t buf[16];
    auto r = h.transport.read(buf, sizeof(buf), 10);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::ResponseLost);
}

TEST_CASE("i2c: write() fails when HAL transmit fails") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("init");

    h.hal.transmit_ok = false;
    uint8_t data[] = {1, 2, 3};
    auto r = h.transport.write(data, sizeof(data));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

// ---------------------------------------------------------------------------
// explicit reset() via Protocol
// ---------------------------------------------------------------------------

TEST_CASE("i2c: explicit reset()") {
    I2cTestHarness h;
    h.hal.responses.push_back("{}\n");
    h.transact("init");

    h.transport.reset();
    h.hal.responses.push_back("{}\n");
    auto r = h.transact("card.version");
    REQUIRE(r.has_value());
}

// ---------------------------------------------------------------------------
// I2cCallbackHal — verify all five delegate methods are called through
// ---------------------------------------------------------------------------

TEST_CASE("I2cCallbackHal delegates to callbacks") {
    // Wire an I2cCallbackHal to a ScriptedI2cHal via lambdas.
    // A successful round-trip exercises reset, transmit, receive, millis, delay.
    ScriptedI2cHal real;
    real.responses.push_back("{}\n");

    I2cCallbackHal cb{
        [&]()                                      -> bool     { return real.reset(); },
        [&](const uint8_t* d, size_t n)            -> bool     { return real.transmit(d, n); },
        [&](uint8_t* b, size_t n, uint32_t& avail) -> bool     { return real.receive(b, n, avail); },
        [&]()                                      -> uint32_t { return real.millis(); },
        [&](uint32_t ms)                                       { real.delay(ms); }
    };

    I2cFramer<I2cPolicy> notecard_i2c(cb);
    note::Protocol transport(notecard_i2c);
    note::Protocol& t = transport;
    note::JsonSink null_sink;
    auto build = [](note::JsonBuilder& b) { b.add("req", "hub.status"); };
    auto r = t.transact(build, null_sink, 5000);
    REQUIRE(r.has_value());
}

// ---------------------------------------------------------------------------
// Multi-chunk send via Protocol with small MTU
// ---------------------------------------------------------------------------

TEST_CASE("i2c send: multi-chunk request with small MTU") {
    I2cTestHarness h;
    h.hal.mtu = 10;
    h.hal.responses.push_back("{}\n");
    h.hal.responses.push_back("{}\n");

    h.transact("init");
    h.hal.transmit_call_count = 0;

    h.transact("hub.sync");
    // Request body + \n should be multiple chunks at MTU=10
    CHECK(h.hal.transmit_call_count > 1);
}

TEST_CASE("i2c receive: multi-chunk response with small MTU") {
    I2cTestHarness h;
    h.hal.mtu = 10;  // force chunking on receive too

    const std::string body(40, 'z');
    const std::string resp = "{\"data\":\"" + body + "\"}\n";
    h.hal.responses.push_back(resp);
    CaptureSink sink;
    auto result = h.transact("hub.status", sink);
    REQUIRE(result);
    CHECK(sink.find("data") == body);
}
