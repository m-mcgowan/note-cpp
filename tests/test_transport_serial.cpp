// Tests for note::transport::NotecardSerial (Hal) and
// note::Protocol protocol logic over serial.
//
// Ported from note-c test/src/_serialNoteReset_test.cpp,
//                        _serialChunkedTransmit_test.cpp,
//                        _serialChunkedReceive_test.cpp,
//             and note-c NoteRequestWithRetry_test.cpp / NoteTransaction_test.cpp.
//
// Key differences from note-c (which uses FFF mocks of C global functions):
//   - We use SerialHal dependency injection (virtual dispatch) instead.
//   - ScriptedHal reacts to transmissions: bare '\n' → injects reset drain
//     bytes; '\r\n' terminator → injects the next queued JSON response.
//     This prevents pre-loaded responses from being consumed during reset.
//
// NotecardSerial is now a Hal (raw byte ops: transmit, read, reset,
// write_line_terminator, delay). Protocol logic (transact, send, retry, CRC)
// lives in Protocol, which wraps a Hal.
//
// When note-c's serial tests change, review the diffs and update accordingly.

#include <doctest.h>

#include <note/transport/serial.hpp>
#include <note/protocol.hpp>

#include <deque>
#include <functional>
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

    // Find the value for a given key and event type.
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

// Helper: string-returning wrapper for char-buffer crc_add (test convenience).
// Used to build CRC-bearing responses that Protocol validates.
inline std::string str_crc_add(const std::string& json, uint16_t seq) {
    char buf[512];
    size_t len = json.size();
    memcpy(buf, json.data(), len);
    size_t new_len = note::transport::detail::crc_add(buf, len, sizeof(buf), seq);
    return std::string(buf, new_len);
}

} // namespace


using namespace note::transport;

// ---------------------------------------------------------------------------
// ScriptedHal — reactive test HAL
//
// Rather than pre-loading both reset and request responses in a shared queue
// (which breaks because the reset drain loop reads ALL available bytes),
// ScriptedHal reacts to what the transport transmits:
//
//   transmit("\n", 1)     → inject reset_drain_response (default "\r\n")
//   transmit("\r\n", 2)   → inject next queued JSON response (if any)
//   other transmits       → append to tx log only
//
// Tests queue JSON responses with queue_response() before calling transact().
// ---------------------------------------------------------------------------

struct ScriptedHal : public SerialHal {
    std::deque<uint8_t> rx;           // bytes the transport can read
    std::deque<uint8_t> tx;           // bytes the transport has sent

    // Responses injected into rx after each request terminator (\r\n).
    std::deque<std::string> json_responses;

    // Response injected into rx after each reset probe (\n).
    // Default is "\r\n" (only control chars — clean reset).
    std::string reset_drain_response = "\r\n";

    // Per-call transmit override: if set, called with the 1-based call number.
    // Return false to simulate a transmit failure.
    std::function<bool(int)> transmit_ok_fn;

    // Clock and delay
    uint32_t now_ms = 0;

    void queue_response(const std::string& s) {
        json_responses.push_back(s);
    }

    // --- SerialHal interface ---

    int transmit_call_ = 0;

    bool transmit(const uint8_t* d, size_t n) override {
        ++transmit_call_;
        if (transmit_ok_fn && !transmit_ok_fn(transmit_call_))
            return false;

        for (size_t i = 0; i < n; ++i) tx.push_back(d[i]);

        if (n == 1 && d[0] == '\n') {
            // Reset probe → inject reset drain response
            for (char c : reset_drain_response) rx.push_back(uint8_t(c));
        } else if (n == 2 && d[0] == '\r' && d[1] == '\n') {
            // Request terminator (from write_line_terminator) → inject next response
            if (!json_responses.empty()) {
                for (char c : json_responses.front()) rx.push_back(uint8_t(c));
                json_responses.pop_front();
            }
        }
        return true;
    }

    size_t receive(uint8_t* buf, size_t max) override {
        size_t n = std::min(max, rx.size());
        for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
        return n;
    }

    uint32_t millis() override { return now_ms; }

    void delay(uint32_t ms) override {
        now_ms += ms;
    }

    std::string take_tx() {
        std::string s(tx.begin(), tx.end());
        tx.clear();
        return s;
    }
};

// ---------------------------------------------------------------------------
// Helper: create a Protocol over NotecardSerial over ScriptedHal.
//
// Returns by value — NotecardSerial and Protocol hold references
// to each other and to the hal, so callers must keep the Harness alive.
// ---------------------------------------------------------------------------

struct SerialTestHarness {
    ScriptedHal hal;
    NotecardSerial<SerialPolicy> notecard_serial;
    note::Protocol transport;

    SerialTestHarness()
        : notecard_serial(hal)
        , transport(notecard_serial)
    {}

    // Convenience: transact with a simple build function and capture sink.
    // Returns Result<void>; captured fields are in the sink.
    note::Result<void> transact(const char* req_name, CaptureSink& sink,
                                uint32_t timeout_ms = 5000) {
        note::IStreamingTransport& t = transport;
        auto build = [&](note::JsonBuilder& b) { b.add("req", req_name); };
        return t.transact(build, sink, timeout_ms);
    }

    // Transact with a NullSink (just check success/failure).
    note::Result<void> transact(const char* req_name,
                                uint32_t timeout_ms = 5000) {
        note::IStreamingTransport& t = transport;
        note::JsonSink null_sink;
        auto build = [&](note::JsonBuilder& b) { b.add("req", req_name); };
        return t.transact(build, null_sink, timeout_ms);
    }

    // Send (fire-and-forget).
    note::Result<void> send(const char* cmd_name) {
        note::IStreamingTransport& t = transport;
        auto build = [&](note::JsonBuilder& b) { b.add("cmd", cmd_name); };
        return t.send(build);
    }
};

// ---------------------------------------------------------------------------
// reset() — ported from note-c _serialNoteReset_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("reset succeeds when drain sees only control characters") {
    // note-c: "Only control character received" → _serialNoteReset() == true
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    auto r = h.transact("hub.set");
    REQUIRE(r.has_value());
}

TEST_CASE("reset: first byte transmitted is bare newline") {
    // note-c: _noteSerialTransmit is called once with a single '\n'
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("hub.set");
    std::string all_tx(h.hal.tx.begin(), h.hal.tx.end());
    REQUIRE(!all_tx.empty());
    REQUIRE(all_tx.front() == '\n');
}

TEST_CASE("reset retries on non-control characters in drain") {
    // note-c: "Non-control character received" + "Retry" → retries and succeeds
    // Use a custom HAL subclass to control per-probe behavior.
    struct RetryHal : public SerialHal {
        std::deque<uint8_t> rx, tx;
        std::deque<std::string> json_responses;
        uint32_t now_ms = 0;
        int probe_num = 0;

        bool transmit(const uint8_t* d, size_t n) override {
            for (size_t i = 0; i < n; ++i) tx.push_back(d[i]);
            if (n == 1 && d[0] == '\n') {
                ++probe_num;
                if (probe_num == 1) {
                    // First probe: inject non-control garbage → fails
                    const char* bad = "garbage\r\n";
                    for (; *bad; ++bad) rx.push_back(uint8_t(*bad));
                } else {
                    // Subsequent: inject clean control → succeeds
                    rx.push_back('\r'); rx.push_back('\n');
                }
            } else if (n == 2 && d[0] == '\r' && d[1] == '\n') {
                if (!json_responses.empty()) {
                    for (char c : json_responses.front()) rx.push_back(uint8_t(c));
                    json_responses.pop_front();
                }
            }
            return true;
        }
        size_t receive(uint8_t* buf, size_t max) override {
            size_t n = std::min(max, rx.size());
            for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
            return n;
        }
        uint32_t millis() override { return now_ms; }
        void delay(uint32_t ms) override { now_ms += ms; }
    } retry_hal;

    retry_hal.json_responses.push_back("{}\r\n");
    NotecardSerial<SerialPolicy> notecard_serial(retry_hal);
    note::Protocol transport(notecard_serial);
    note::IStreamingTransport& t = transport;
    note::JsonSink null_sink;
    auto build = [](note::JsonBuilder& b) { b.add("req", "hub.set"); };
    auto r = t.transact(build, null_sink, 5000);
    REQUIRE(r.has_value());
    REQUIRE(retry_hal.probe_num >= 2);
}

TEST_CASE("reset fails if all attempts see non-control chars") {
    // note-c: "Serial never available" → _serialNoteReset() == false
    SerialTestHarness h;
    h.hal.reset_drain_response = "BAD\r\n";  // always injects non-control data
    auto r = h.transact("hub.set");
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

// ---------------------------------------------------------------------------
// Transmit framing — write_line_terminator appends CRLF
//
// Note: The old NotecardSerial did segmented TX with pacing delays. The new
// NotecardSerial is a raw Hal — no segmenting. Protocol
// writes the JSON body field-by-field, then calls write_line_terminator().
// Segmented transmit tests are no longer applicable.
// ---------------------------------------------------------------------------

TEST_CASE("request is terminated with CRLF") {
    // After JSON body, write_line_terminator sends \r\n.
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("hub.set");

    std::string tx(h.hal.tx.begin(), h.hal.tx.end());
    // Find last \r\n — should be the request terminator
    auto last_crlf = tx.rfind("\r\n");
    REQUIRE(last_crlf != std::string::npos);
    // There should be JSON content before the \r\n
    auto json_start = tx.find('{');
    REQUIRE(json_start != std::string::npos);
    REQUIRE(json_start < last_crlf);
}

TEST_CASE("request JSON contains the req field") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("hub.set");

    std::string tx(h.hal.tx.begin(), h.hal.tx.end());
    // The JSON body should contain "req" and "hub.set"
    REQUIRE(tx.find("\"req\"") != std::string::npos);
    REQUIRE(tx.find("hub.set") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Response parsing — ported from note-c _serialChunkedReceive_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("response JSON fields are delivered to sink") {
    // note-c: packet received → correct bytes returned
    SerialTestHarness h;
    h.hal.queue_response("{\"connected\":true}\r\n");
    CaptureSink sink;
    auto r = h.transact("hub.set", sink);
    REQUIRE(r.has_value());
    CHECK(sink.find("connected", "bool") == "true");
}

TEST_CASE("response with bare LF terminator is parsed") {
    SerialTestHarness h;
    h.hal.queue_response("{\"ok\":true}\n");
    CaptureSink sink;
    auto r = h.transact("hub.set", sink);
    REQUIRE(r.has_value());
    CHECK(sink.find("ok", "bool") == "true");
}

TEST_CASE("response timeout before first byte returns error") {
    // note-c: _noteSerialAvailable always false -> timeout -> error
    struct NoDataHal : public SerialHal {
        uint32_t now_ms = 0;
        std::deque<uint8_t> rx;

        bool transmit(const uint8_t* d, size_t n) override {
            if (n == 1 && d[0] == '\n') {
                rx.push_back('\r'); rx.push_back('\n');
            }
            return true;
        }
        size_t receive(uint8_t* buf, size_t max) override {
            size_t n = std::min(max, rx.size());
            for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
            return n;
        }
        uint32_t millis() override { return now_ms; }
        // Fast-forward time so timeout fires quickly
        void delay(uint32_t ms) override { now_ms += ms + 100; }
    } hal;

    NotecardSerial<SerialPolicy> notecard_serial(hal);
    note::Protocol transport(notecard_serial);
    note::IStreamingTransport& t = transport;
    note::JsonSink null_sink;
    auto build = [](note::JsonBuilder& b) { b.add("req", "hub.set"); };
    auto r = t.transact(build, null_sink, 500);
    REQUIRE(!r.has_value());
    // Timeout on the wire is a transport-level error: ResponseLost.
    REQUIRE(r.error().code == note::Error::ResponseLost);
}

TEST_CASE("response timeout after partial data") {
    // note-c: bytes intermittently available but no terminator, timeout
    struct PartialHal : public SerialHal {
        uint32_t now_ms = 0;
        std::deque<uint8_t> rx;

        bool transmit(const uint8_t* d, size_t n) override {
            if (n == 1 && d[0] == '\n') {
                rx.push_back('\r'); rx.push_back('\n');
            }
            // After CRLF terminator, give one byte of partial JSON
            if (n == 2 && d[0] == '\r' && d[1] == '\n') {
                rx.push_back('{');
            }
            return true;
        }
        size_t receive(uint8_t* buf, size_t max) override {
            size_t n = std::min(max, rx.size());
            for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
            return n;
        }
        uint32_t millis() override { return now_ms; }
        // Fast-forward time
        void delay(uint32_t ms) override { now_ms += ms + 200; }
    } hal;

    NotecardSerial<SerialPolicy> notecard_serial(hal);
    note::Protocol transport(notecard_serial);
    note::IStreamingTransport& t = transport;
    note::JsonSink null_sink;
    auto build = [](note::JsonBuilder& b) { b.add("req", "hub.set"); };
    auto r = t.transact(build, null_sink, 10000);
    REQUIRE(!r.has_value());
    // Partial data + timeout is a transport-level error: ResponseLost.
    REQUIRE(r.error().code == note::Error::ResponseLost);
}

// ---------------------------------------------------------------------------
// Full round-trip
// ---------------------------------------------------------------------------

TEST_CASE("full round-trip: request transmitted, response parsed") {
    SerialTestHarness h;
    h.hal.queue_response("{\"version\":\"1.0\"}\r\n");
    CaptureSink sink;
    auto r = h.transact("card.version", sink);
    REQUIRE(r.has_value());
    CHECK(sink.find("version") == "1.0");
}

TEST_CASE("second call reuses existing connection without reset") {
    // note-c: initialized_ = true after first call, no re-reset
    SerialTestHarness h;

    h.hal.queue_response("{\"ok\":true}\r\n");
    auto r1 = h.transact("hub.set");
    REQUIRE(r1.has_value());

    size_t tx_size_after_first = h.hal.tx.size();

    h.hal.queue_response("{\"ok\":true}\r\n");
    auto r2 = h.transact("hub.sync");
    REQUIRE(r2.has_value());

    // Second call should NOT have sent another reset probe ('\n' alone)
    // The tx from the second call should start directly with the JSON
    std::string all_tx(h.hal.tx.begin() + static_cast<std::ptrdiff_t>(tx_size_after_first), h.hal.tx.end());
    REQUIRE(all_tx.front() == '{');  // starts with JSON, not reset probe
}

// ---------------------------------------------------------------------------
// CRC auto-detection and validation — via Protocol
//
// Protocol handles CRC internally via CrcFieldSink + CrcAccumulator.
// These tests verify the full CRC path through the streaming transport.
// ---------------------------------------------------------------------------

TEST_CASE("CRC auto-detection: no CRC in response, no error") {
    SerialTestHarness h;
    h.hal.queue_response("{\"connected\":true}\r\n");
    CaptureSink sink;
    auto r = h.transact("hub.set", sink);
    REQUIRE(r.has_value());
    CHECK(sink.find("connected", "bool") == "true");
}

TEST_CASE("CRC auto-detection: first response with CRC enables CRC") {
    SerialTestHarness h;

    // CRC is always sent. crc_seq_ starts at 0, incremented before send,
    // so the first request uses seq=1.
    std::string resp = str_crc_add("{\"ok\":true}", 1) + "\r\n";
    h.hal.queue_response(resp);
    CaptureSink sink;
    auto r = h.transact("hub.set", sink);
    REQUIRE(r.has_value());
    CHECK(sink.find("ok", "bool") == "true");
    // CRC field should not appear in the user sink (intercepted by CrcFieldSink)
    CHECK_FALSE(sink.has_key("crc"));
}

TEST_CASE("CRC: after detection, second request includes CRC field") {
    SerialTestHarness h;

    // First call: seq=1 (always sent)
    h.hal.queue_response(str_crc_add("{\"ok\":true}", 1) + "\r\n");
    h.transact("hub.set");

    // Second call: seq=2
    h.hal.queue_response(str_crc_add("{\"ok\":true}", 2) + "\r\n");
    h.hal.tx.clear();
    h.transact("hub.sync");

    std::string tx(h.hal.tx.begin(), h.hal.tx.end());
    REQUIRE(tx.find("\"crc\":\"") != std::string::npos);
}

TEST_CASE("CRC mismatch returns ResponseLost") {
    SerialTestHarness h;

    // First call: seq=1
    h.hal.queue_response(str_crc_add("{\"ok\":true}", 1) + "\r\n");
    h.transact("hub.set");

    // Second call: response has wrong seq -> CRC mismatch -> error (no retry).
    h.hal.queue_response(str_crc_add("{\"ok\":true}", 99) + "\r\n");  // wrong seq
    CaptureSink sink;
    auto r = h.transact("hub.sync", sink);
    REQUIRE(!r.has_value());
    CHECK(r.error().code == note::Error::ResponseLost);
    CHECK(r.error().cause == note::Cause::CrcMismatch);
}

// ---------------------------------------------------------------------------
// Retry on I/O error — ported from note-c NoteRequestWithRetry_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("I/O error on transmit returns SendFailed") {
    SerialTestHarness h;
    // The transmit_ok_fn applies to the SerialHal-level transmit calls.
    // Call 1: reset probe '\n' (succeeds)
    // Call 2: first JSON body fragment -> fails
    // Transport returns SendFailed immediately (no retry).
    h.hal.transmit_ok_fn = [](int call) -> bool { return call != 2; };

    h.hal.queue_response("{\"ok\":true}\r\n");
    auto r = h.transact("hub.set");
    REQUIRE(!r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

TEST_CASE("reset failure returns Error::NotReady") {
    SerialTestHarness h;
    h.hal.reset_drain_response = "BAD\r\n";  // reset always fails
    auto r = h.transact("hub.set");
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

// ---------------------------------------------------------------------------
// SerialCallbackHal — verify all delegate methods are called through
// ---------------------------------------------------------------------------

TEST_CASE("SerialCallbackHal delegates to callbacks") {
    // Wire a SerialCallbackHal to a ScriptedHal via lambdas.
    // A successful round-trip exercises transmit, receive, millis, and delay.
    ScriptedHal real;
    real.queue_response("{}\r\n");

    SerialCallbackHal cb{
        [&](const uint8_t* d, size_t n)  -> bool     { return real.transmit(d, n); },
        [&](uint8_t* b,       size_t n)  -> size_t   { return real.receive(b, n); },
        [&]()                            -> uint32_t  { return real.millis(); },
        [&](uint32_t ms)                             { real.delay(ms); }
    };

    NotecardSerial<SerialPolicy> notecard_serial(cb);
    note::Protocol transport(notecard_serial);
    note::IStreamingTransport& t = transport;
    note::JsonSink null_sink;
    auto build = [](note::JsonBuilder& b) { b.add("req", "hub.status"); };
    auto r = t.transact(build, null_sink, 5000);
    REQUIRE(r.has_value());
}

// ---------------------------------------------------------------------------
// Binary write/read — raw byte streaming via Protocol
// ---------------------------------------------------------------------------

TEST_CASE("serial: write() sends raw bytes without framing") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("card.binary.put");  // init
    h.hal.tx.clear();

    uint8_t data[] = {0x01, 0x02, 0x0A, 0x03};
    auto r = h.transport.write(data, sizeof(data));
    REQUIRE(r.has_value());

    // Raw bytes — no \r\n appended
    REQUIRE(h.hal.tx.size() == 4);
    CHECK(h.hal.tx[0] == 0x01);
    CHECK(h.hal.tx[2] == 0x0A);
}

TEST_CASE("serial: read() returns available bytes") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("card.binary.get");  // init

    // Inject some bytes to read
    h.hal.rx.push_back(0xAA);
    h.hal.rx.push_back(0xBB);
    h.hal.rx.push_back(0x0A);

    uint8_t buf[16];
    auto r = h.transport.read(buf, sizeof(buf), 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == 3);
    CHECK(buf[0] == 0xAA);
    CHECK(buf[1] == 0xBB);
    CHECK(buf[2] == 0x0A);
}

TEST_CASE("serial: read() times out when no data") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("init");  // init

    // No bytes in rx — should timeout
    uint8_t buf[16];
    auto r = h.transport.read(buf, sizeof(buf), 10);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::ResponseLost);
}

// ---------------------------------------------------------------------------
// send() — fire-and-forget via Protocol
// ---------------------------------------------------------------------------

TEST_CASE("serial: send() transmits without waiting for response") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("init");  // init
    h.hal.tx.clear();

    auto r = h.send("hub.set");
    REQUIRE(r.has_value());
    auto sent = h.hal.take_tx();
    CHECK(sent.find("hub.set") != std::string::npos);
}

TEST_CASE("serial: explicit reset()") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("init");  // init

    h.transport.reset();
    h.hal.queue_response("{}\r\n");
    auto r = h.transact("card.version");
    REQUIRE(r.has_value());
}

TEST_CASE("serial: send() before first transact triggers reset") {
    SerialTestHarness h;
    // No transact() — transport is uninitialized, send() must auto-reset.
    auto r = h.send("hub.set");
    REQUIRE(r.has_value());
}

TEST_CASE("serial: send() fails when transmit fails") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("init");

    // Make transmit fail
    h.hal.transmit_ok_fn = [](int) { return false; };
    auto r = h.send("hub.set");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

TEST_CASE("serial: write() fails when HAL transmit fails") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");
    h.transact("init");

    h.hal.transmit_ok_fn = [](int) { return false; };
    uint8_t data[] = {1, 2, 3};
    auto r = h.transport.write(data, sizeof(data));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

TEST_CASE("serial: transact retries on transmit failure then succeeds") {
    SerialTestHarness h;
    h.hal.queue_response("{}\r\n");  // consumed by first attempt
    h.hal.queue_response("{}\r\n");  // the actual response after retry

    // Fail initial transmits, succeed later
    int call = 0;
    h.hal.transmit_ok_fn = [&](int) {
        ++call;
        // Fail the first few transmit calls of the request body,
        // succeed all subsequent attempts
        return call > 2;
    };

    auto r = h.transact("hub.status");
    REQUIRE(r.has_value());
}

// ---------------------------------------------------------------------------
// HAL-level tests — test NotecardSerial directly as Hal
// ---------------------------------------------------------------------------

TEST_CASE("NotecardSerial::reset() succeeds on clean drain") {
    ScriptedHal hal;
    NotecardSerial<SerialPolicy> serial(hal);
    REQUIRE(serial.reset() == true);
}

TEST_CASE("NotecardSerial::reset() fails when drain has non-control chars") {
    ScriptedHal hal;
    hal.reset_drain_response = "GARBAGE\r\n";
    NotecardSerial<SerialPolicy> serial(hal);
    REQUIRE(serial.reset() == false);
}

TEST_CASE("NotecardSerial::transmit() forwards to SerialHal") {
    ScriptedHal hal;
    NotecardSerial<SerialPolicy> serial(hal);
    uint8_t data[] = {0x41, 0x42, 0x43};
    REQUIRE(serial.transmit(data, 3) == true);
    REQUIRE(hal.tx.size() == 3);
    CHECK(hal.tx[0] == 0x41);
    CHECK(hal.tx[1] == 0x42);
    CHECK(hal.tx[2] == 0x43);
}

TEST_CASE("NotecardSerial::transmit() returns false on HAL failure") {
    ScriptedHal hal;
    hal.transmit_ok_fn = [](int) { return false; };
    NotecardSerial<SerialPolicy> serial(hal);
    uint8_t data[] = {0x41};
    REQUIRE(serial.transmit(data, 1) == false);
}

TEST_CASE("NotecardSerial::write_line_terminator() sends CRLF") {
    ScriptedHal hal;
    NotecardSerial<SerialPolicy> serial(hal);
    REQUIRE(serial.write_line_terminator() == true);
    REQUIRE(hal.tx.size() == 2);
    CHECK(hal.tx[0] == '\r');
    CHECK(hal.tx[1] == '\n');
}

TEST_CASE("NotecardSerial::read() returns data from SerialHal") {
    ScriptedHal hal;
    hal.rx.push_back(0xAA);
    hal.rx.push_back(0xBB);
    NotecardSerial<SerialPolicy> serial(hal);
    uint8_t buf[8];
    auto r = serial.read(buf, sizeof(buf), 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == 2);
    CHECK(buf[0] == 0xAA);
    CHECK(buf[1] == 0xBB);
}

TEST_CASE("NotecardSerial::read() times out when no data available") {
    ScriptedHal hal;
    NotecardSerial<SerialPolicy> serial(hal);
    uint8_t buf[8];
    auto r = serial.read(buf, sizeof(buf), 10);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::ResponseLost);
}

TEST_CASE("NotecardSerial::delay() forwards to SerialHal") {
    ScriptedHal hal;
    NotecardSerial<SerialPolicy> serial(hal);
    serial.delay(42);
    CHECK(hal.now_ms == 42);
}
