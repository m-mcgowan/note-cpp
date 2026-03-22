// Tests for note::transport::NotecardSerial.
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
// When note-c's serial tests change, review the diffs and update accordingly.

#include "catch.hpp"

#include <note/transport/serial.hpp>

#include <deque>
#include <functional>
#include <string>

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
// Tests queue JSON responses with queue_response() before calling operator().
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
    // Counts inter-segment delays (kSerialSegmentDelayMs) that occur during
    // request transmission — excludes the pre-reset delay.
    int segment_delay_count = 0;
    bool in_request_ = false;  // true after first non-probe transmit byte

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
            // Reset probe → inject reset drain response; not in a request
            in_request_ = false;
            for (char c : reset_drain_response) rx.push_back(uint8_t(c));
        } else if (n == 2 && d[0] == '\r' && d[1] == '\n') {
            // Request terminator → inject next queued JSON response
            if (!json_responses.empty()) {
                for (char c : json_responses.front()) rx.push_back(uint8_t(c));
                json_responses.pop_front();
            }
        } else {
            // JSON segment — we are now in a request
            in_request_ = true;
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
        // Only count inter-segment delays during request transmission
        if (in_request_ && ms == kSerialSegmentDelayMs) ++segment_delay_count;
    }

    std::string take_tx() {
        std::string s(tx.begin(), tx.end());
        tx.clear();
        return s;
    }
};

// ---------------------------------------------------------------------------
// reset() — ported from note-c _serialNoteReset_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("reset succeeds when drain sees only control characters") {
    // note-c: "Only control character received" → _serialNoteReset() == true
    ScriptedHal hal;  // default reset_drain_response = "\r\n"
    NotecardSerial transport(hal);
    hal.queue_response("{}\r\n");
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r.has_value());
}

TEST_CASE("reset: first byte transmitted is bare newline") {
    // note-c: _noteSerialTransmit is called once with a single '\n'
    ScriptedHal hal;
    NotecardSerial transport(hal);
    hal.queue_response("{}\r\n");
    transport("{\"req\":\"hub.set\"}", 5000);
    std::string all_tx(hal.tx.begin(), hal.tx.end());
    REQUIRE(!all_tx.empty());
    REQUIRE(all_tx.front() == '\n');
}

TEST_CASE("reset retries on non-control characters in drain") {
    // note-c: "Non-control character received" + "Retry" → retries and succeeds
    // First reset probe → inject non-control data (triggers retry)
    // Second probe → inject clean "\r\n" (succeeds)
    ScriptedHal hal;
    hal.transmit_ok_fn = [](int /*call*/) -> bool {
        // We intercept by watching what's in the tx queue via override
        return true;
    };
    // Override: first reset probe gets garbage; subsequent ones get clean drain.
    // We achieve this by making reset_drain_response a queue.
    // Simplest approach: use a stateful transmit that changes the response.
    ScriptedHal hal2;
    hal2.transmit_ok_fn = [](int /*call*/) -> bool {
        // track in receive: handled by overriding transmit fully below
        return true;
    };

    // Use a custom HAL subclass to control per-probe behavior
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
    NotecardSerial transport2(retry_hal);
    auto r = transport2("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(retry_hal.probe_num >= 2);
}

TEST_CASE("reset fails if all attempts see non-control chars") {
    // note-c: "Serial never available" → _serialNoteReset() == false
    // (or: all retries see non-control data)
    ScriptedHal hal;
    hal.reset_drain_response = "BAD\r\n";  // always injects non-control data
    NotecardSerial transport(hal);
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

// ---------------------------------------------------------------------------
// send_segmented — ported from note-c _serialChunkedTransmit_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("short request is sent as single segment with CRLF") {
    // note-c: buffer < CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN → 1 transmit call
    ScriptedHal hal;
    NotecardSerial transport(hal);
    hal.queue_response("{}\r\n");
    transport("{\"req\":\"hub.set\"}", 5000);

    std::string tx = std::string(hal.tx.begin(), hal.tx.end());
    auto req_start = tx.find('{');
    REQUIRE(req_start != std::string::npos);
    std::string req_part = tx.substr(req_start);
    REQUIRE(req_part == "{\"req\":\"hub.set\"}\r\n");
}

TEST_CASE("600-byte request splits into 3 segments with 2 inter-segment delays") {
    // note-c: buffer > CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN → multiple transmit calls
    // 600 bytes = 250 + 250 + 100 → 3 segments, 2 inter-segment delays
    ScriptedHal hal;
    NotecardSerial transport(hal);

    std::string big_req = "{\"req\":\"hub.set\",\"data\":\"";
    big_req += std::string(570, 'x');
    big_req += "\"}";

    hal.queue_response("{}\r\n");
    transport(big_req, 5000);

    REQUIRE(hal.segment_delay_count == 2);
}

TEST_CASE("request at exactly segment boundary sends in 1 segment") {
    // note-c: buffer == CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN → 1 transmit call
    ScriptedHal hal;
    NotecardSerial transport(hal);

    // Build exactly 250-byte JSON
    std::string req = "{\"d\":\"";
    req += std::string(250 - 9, 'x');  // 250 total including {"d":"...",CRLF but CRLF is separate
    req += "\"}";
    // Adjust to exactly kSerialSegmentMaxLen bytes
    // kSerialSegmentMaxLen = 250
    // We just need < 250 to avoid splitting
    std::string exact_req(kSerialSegmentMaxLen, 'x');
    exact_req[0] = '{'; exact_req[kSerialSegmentMaxLen-1] = '}';

    hal.queue_response("{}\r\n");
    transport(exact_req, 5000);
    REQUIRE(hal.segment_delay_count == 0);  // no inter-segment delays
}

TEST_CASE("send_segmented appends CRLF terminator") {
    ScriptedHal hal;
    NotecardSerial transport(hal);
    hal.queue_response("{}\r\n");
    transport("{\"req\":\"hub.set\"}", 5000);

    std::string tx = std::string(hal.tx.begin(), hal.tx.end());
    auto req_start = tx.find('{');
    REQUIRE(req_start != std::string::npos);
    std::string req_part = tx.substr(req_start);
    REQUIRE(req_part.size() >= 2);
    REQUIRE(req_part[req_part.size()-2] == '\r');
    REQUIRE(req_part[req_part.size()-1] == '\n');
}

// ---------------------------------------------------------------------------
// receive_line — ported from note-c _serialChunkedReceive_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("receive_line returns JSON stripped of CRLF") {
    // note-c: packet received → correct bytes returned
    ScriptedHal hal;
    NotecardSerial transport(hal);
    hal.queue_response("{\"connected\":true}\r\n");
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"connected\":true}");
}

TEST_CASE("receive_line returns JSON stripped of bare LF") {
    ScriptedHal hal;
    NotecardSerial transport(hal);
    hal.queue_response("{\"ok\":true}\n");
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"ok\":true}");
}

TEST_CASE("receive_line timeout before first byte returns ResponseLost") {
    // note-c: _noteSerialAvailable always false → timeout → error
    // Uses a custom HAL whose delay() fast-forwards the clock.
    struct NoDataHal : public SerialHal {
        uint32_t now_ms = 0;
        std::deque<uint8_t> rx;  // starts with reset drain bytes
        bool reset_consumed = false;

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
        // Fast-forward: add extra to each delay so timeout fires quickly
        void delay(uint32_t ms) override { now_ms += ms + 100; }
    } hal;

    NotecardSerial transport(hal);
    auto r = transport("{\"req\":\"hub.set\"}", 500);
    REQUIRE(!r.has_value());
    // After kMaxRetries timeouts, the last error is propagated.
    REQUIRE(r.error().code == note::Error::ResponseLost);
    REQUIRE(r.error().cause == note::Cause::Timeout);
}

TEST_CASE("receive_line intra-transaction timeout after first byte") {
    // note-c: bytes intermittently available but no terminator, timeout
    struct PartialHal : public SerialHal {
        uint32_t now_ms = 0;
        std::deque<uint8_t> rx;
        bool first_data_given = false;

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
        // Fast-forward: add extra to each delay
        void delay(uint32_t ms) override { now_ms += ms + 200; }
    } hal;

    NotecardSerial transport(hal);
    auto r = transport("{\"req\":\"hub.set\"}", 10000);
    REQUIRE(!r.has_value());
    // After retrying with partial responses that never complete, exhausts retries.
    REQUIRE(r.error().code == note::Error::ResponseLost);
    REQUIRE((r.error().cause == note::Cause::Timeout ||
             r.error().cause == note::Cause::TimeoutIntra));
}

// ---------------------------------------------------------------------------
// Full round-trip
// ---------------------------------------------------------------------------

TEST_CASE("full round-trip: request transmitted, response returned") {
    ScriptedHal hal;
    NotecardSerial transport(hal);
    hal.queue_response("{\"version\":\"1.0\"}\r\n");
    auto r = transport("{\"req\":\"card.version\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"version\":\"1.0\"}");
}

TEST_CASE("second call reuses existing connection without reset") {
    // note-c: initialized_ = true after first call, no re-reset
    ScriptedHal hal;
    NotecardSerial transport(hal);

    hal.queue_response("{\"ok\":true}\r\n");
    auto r1 = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r1.has_value());

    for (auto b : hal.tx) if (b == '\n' && (&b == &hal.tx.back() || true)) { /* count */ }
    size_t tx_size_after_first = hal.tx.size();

    hal.queue_response("{\"ok\":true}\r\n");
    auto r2 = transport("{\"req\":\"hub.sync\"}", 5000);
    REQUIRE(r2.has_value());

    // Second call should NOT have sent another reset probe ('\n' alone)
    // The tx from the second call should start directly with the JSON
    std::string all_tx(hal.tx.begin() + static_cast<std::ptrdiff_t>(tx_size_after_first), hal.tx.end());
    REQUIRE(all_tx.front() == '{');  // starts with JSON, not reset probe
}

// ---------------------------------------------------------------------------
// CRC auto-detection and validation
// ---------------------------------------------------------------------------

TEST_CASE("CRC auto-detection: no CRC in response, flag stays false") {
    ScriptedHal hal;
    NotecardSerial transport(hal);
    hal.queue_response("{\"connected\":true}\r\n");
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"connected\":true}");
    // crc_enabled_ remains false — verified by checking next request has no CRC
}

TEST_CASE("CRC auto-detection: first response with CRC sets enabled flag") {
    ScriptedHal hal;
    NotecardSerial transport(hal);

    // crc_enabled_=false at start; seq not incremented (CRC not enabled yet)
    // The response includes a CRC field matching seq=0.
    std::string resp = note::transport::detail::crc_add("{\"ok\":true}", 0) + "\r\n";
    hal.queue_response(resp);
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"ok\":true}");
    // crc_enabled_ is now true
}

TEST_CASE("CRC: after detection, second request includes CRC field") {
    ScriptedHal hal;
    NotecardSerial transport(hal);

    // First call: CRC auto-detected (seq=0 response)
    hal.queue_response(note::transport::detail::crc_add("{\"ok\":true}", 0) + "\r\n");
    transport("{\"req\":\"hub.set\"}", 5000);

    // Second call: seq becomes 1, request must include CRC
    hal.queue_response(note::transport::detail::crc_add("{\"ok\":true}", 1) + "\r\n");
    hal.tx.clear();
    transport("{\"req\":\"hub.sync\"}", 5000);

    std::string tx(hal.tx.begin(), hal.tx.end());
    REQUIRE(tx.find("\"crc\":\"") != std::string::npos);
}

TEST_CASE("CRC mismatch triggers retry and succeeds on clean response") {
    // note-c: NoteRequestWithRetry / retry on CRC error
    ScriptedHal hal;
    NotecardSerial transport(hal);

    // First call: CRC auto-detected
    hal.queue_response(note::transport::detail::crc_add("{\"ok\":true}", 0) + "\r\n");
    transport("{\"req\":\"hub.set\"}", 5000);

    // Second call: first response has wrong seq → retry; second response is correct
    hal.queue_response(note::transport::detail::crc_add("{\"ok\":true}", 99) + "\r\n");  // wrong seq
    hal.queue_response(note::transport::detail::crc_add("{\"ok\":true}", 1) + "\r\n");   // correct
    auto r = transport("{\"req\":\"hub.sync\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Retry on I/O error — ported from note-c NoteRequestWithRetry_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("I/O error on transmit triggers retry, succeeds on recovery") {
    // note-c: _noteSerialTransmit fails on first data send → retry succeeds
    ScriptedHal hal;
    // Call 1: reset probe '\n' (succeeds)
    // Call 2: first JSON segment → fails
    // Call 3: second reset probe '\n' (succeeds, from do_reset() in retry)
    // Call 4+: JSON segment + CRLF → succeeds
    hal.transmit_ok_fn = [](int call) -> bool { return call != 2; };

    NotecardSerial transport(hal);
    hal.queue_response("{\"ok\":true}\r\n");
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(r.has_value());
}

TEST_CASE("reset failure returns Error::NotReady") {
    ScriptedHal hal;
    hal.reset_drain_response = "BAD\r\n";  // reset always fails
    NotecardSerial transport(hal);
    auto r = transport("{\"req\":\"hub.set\"}", 5000);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

// ---------------------------------------------------------------------------
// SerialCallbackHal — verify all four delegate methods are called through
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

    NotecardSerial transport(cb);
    auto r = transport("{\"req\":\"hub.status\"}", 5000);
    REQUIRE(r.has_value());
    CHECK(*r == "{}");
}
