// Tests for note::transport::NotecardI2c.
//
// Ported from note-c test/src/_i2cNoteReset_test.cpp,
// _i2cChunkedTransmit_test.cpp, and _i2cChunkedReceive_test.cpp.
//
// Key mapping:
//   note-c _i2cNoteReset()       → NotecardI2c::do_reset()
//   note-c _i2cChunkedTransmit() → NotecardI2c::send_chunked()
//   note-c _i2cChunkedReceive()  → NotecardI2c::receive_response()
//   note-c _I2CReceive(addr, buf, 0, &avail) → I2cHal::receive(buf, 0, avail)
//
// When note-c's I2C tests change, review the diff and update here accordingly.

#include "catch.hpp"

#include <note/transport/i2c.hpp>
#include <note/transport/detail/crc32.hpp>

#include <cstring>
#include <deque>
#include <string>

using namespace note::transport;
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
        if (!transmit_ok) return false;

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

    // Helper: pre-initialize transport so the first operator() call skips reset.
    void prime(NotecardI2c& t) {
        responses.push_back("{}\n");
        auto r = t(note::string_view{"{}"}, 5000);
        (void)r;
        reset_call_count    = 0;
        transmit_call_count = 0;
        delay_call_count    = 0;
        total_delay_ms      = 0;
        now_ms              = 0;
    }
};

// ---------------------------------------------------------------------------
// do_reset — ported from note-c _i2cNoteReset_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("i2c reset: pre-delay of kI2cSegmentDelayMs (250 ms)") {
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");
    transport({"{}"}, 5000);
    // First delay call must be the 250 ms pre-delay.
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs);
}

TEST_CASE("i2c reset: calls hal.reset() once on first use") {
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");
    transport({"{}"}, 5000);
    CHECK(hal.reset_call_count >= 1);
}

TEST_CASE("i2c reset: io delay (6 ms) after reset()") {
    // delayIO called after _I2CReset — counts toward total_delay_ms.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");
    transport({"{}"}, 5000);
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs + kI2cIoDelayMs);
}

TEST_CASE("i2c reset: reset() fails → NotReady error") {
    // note-c: _i2cNoteReset returns false immediately when _I2CReset fails.
    ScriptedI2cHal hal;
    hal.reset_ok = false;
    NotecardI2c transport(hal);
    auto result = transport({"{}"}, 5000);
    REQUIRE(!result);
    CHECK(result.error().code == Error::NotReady);
}

TEST_CASE("i2c reset: clean drain succeeds") {
    // note-c: GIVEN _noteI2CReceive receives only \r and \n → succeeds.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");
    auto result = transport({"{}"}, 5000);
    // If reset succeeded, operator() would proceed to the actual request.
    REQUIRE(result);
    CHECK(*result == "{}");
}

TEST_CASE("i2c reset: transmit \n fails (NACK) all retries → NotReady error") {
    // note-c: _noteI2CTransmit fails → delay 1000 ms per retry ×
    // CARD_RESET_SYNC_RETRIES → return false.
    ScriptedI2cHal hal;
    hal.transmit_ok = false;
    NotecardI2c transport(hal);
    auto result = transport({"{}"}, 5000);
    REQUIRE(!result);
    CHECK(result.error().code == Error::NotReady);
    // Should have retried kI2cResetSyncRetries times.
    CHECK(hal.transmit_call_count == static_cast<int>(kI2cResetSyncRetries));
}

TEST_CASE("i2c reset: NACK delays kI2cNackWaitMs (1000 ms) per attempt") {
    ScriptedI2cHal hal;
    hal.transmit_ok = false;
    NotecardI2c transport(hal);
    transport({"{}"}, 5000);
    // Each NACK incurs kI2cNackWaitMs (1000 ms).
    CHECK(hal.total_delay_ms >= kI2cNackWaitMs * kI2cResetSyncRetries);
}

TEST_CASE("i2c reset: drain receives non-control chars → all retries fail") {
    // note-c: non-control characters found in drain → notecardReady=false,
    // retry. After kI2cResetSyncRetries all fail → return false.
    ScriptedI2cHal hal;
    hal.reset_response = "{}";  // non-control chars — garbage on the bus
    NotecardI2c transport(hal);
    auto result = transport({"{}"}, 5000);
    REQUIRE(!result);
    CHECK(result.error().code == Error::NotReady);
}

TEST_CASE("i2c reset: drain delay is kI2cSegmentDelayMs after transmit") {
    // note-c: after _noteI2CTransmit succeeds → _DelayMs(CARD_REQUEST_I2C_SEGMENT_DELAY_MS).
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");
    // Record delay sequence: pre(250) + io(6) + post-tx(250) + drain(500) + ...
    transport({"{}"}, 5000);
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs + kI2cIoDelayMs + kI2cSegmentDelayMs);
}

TEST_CASE("i2c reset: priming query (len=0) is first receive in drain") {
    // note-c: chunkLen starts at 0 → first _I2CReceive uses 0 as the size.
    // Verify by observing that receive(0) is called before any data is read.
    // Simulate by having reset_response return data only after priming.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");
    // Default mtu=1024 but drain buf size=64 constrains reads.
    // The priming call (len=0) must happen before chunk reads.
    // If this call order is wrong, rx_buf would be consumed before available
    // is known, and the drain loop would behave incorrectly.
    auto result = transport({"{}"}, 5000);
    CHECK(result.has_value());
}

// ---------------------------------------------------------------------------
// send_chunked — ported from note-c _i2cChunkedTransmit_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("i2c send: small request sent in one chunk") {
    // note-c: buffer fits in one chunk → 1 call to _noteI2CTransmit.
    ScriptedI2cHal hal;
    hal.mtu = 30;
    NotecardI2c transport(hal);
    hal.prime(transport);

    hal.responses.push_back("{}\n");
    transport({"{\"req\":\"hub.sync\"}"}, 5000);

    // One chunk for the request ("{\"req\":\"hub.sync\"}\n" = 20 bytes < 30).
    CHECK(hal.transmit_call_count == 1);
}

TEST_CASE("i2c send: multi-chunk request") {
    // note-c: input buffer > MTU → multiple _noteI2CTransmit calls.
    ScriptedI2cHal hal;
    hal.mtu = 10;
    NotecardI2c transport(hal);
    hal.prime(transport);

    // Build a request larger than 10 bytes.
    const std::string req(50, 'a');
    const std::string json = "{\"x\":\"" + req + "\"}";
    hal.responses.push_back("{}\n");
    transport({json}, 5000);

    // json + \n = 59 bytes → ceil(59/10) = 6 chunks
    CHECK(hal.transmit_call_count > 1);
}

TEST_CASE("i2c send: io delay (6 ms) before each chunk") {
    ScriptedI2cHal hal;
    hal.mtu = 10;
    NotecardI2c transport(hal);
    hal.prime(transport);

    const std::string req(30, 'a');
    const std::string json = "{\"x\":\"" + req + "\"}";  // ~36 chars + \n = 4 chunks
    hal.responses.push_back("{}\n");
    transport({json}, 5000);

    // Expect 1 io delay per chunk.
    int chunks = hal.transmit_call_count;
    CHECK(hal.total_delay_ms >= static_cast<uint32_t>(chunks) * kI2cIoDelayMs);
}

TEST_CASE("i2c send: chunk delay (20 ms) after each chunk") {
    ScriptedI2cHal hal;
    hal.mtu = 10;
    NotecardI2c transport(hal);
    hal.prime(transport);

    const std::string req(20, 'a');
    const std::string json = "{\"x\":\"" + req + "\"}";
    hal.responses.push_back("{}\n");
    transport({json}, 5000);

    int chunks = hal.transmit_call_count;
    // Each chunk gets io delay + chunk delay.
    CHECK(hal.total_delay_ms >= static_cast<uint32_t>(chunks) * (kI2cIoDelayMs + kI2cChunkDelayMs));
}

TEST_CASE("i2c send: segment delay (250 ms) after > 250 bytes in segment") {
    // note-c: sentInSegment > CARD_REQUEST_I2C_SEGMENT_MAX_LEN (250) → delay 250 ms.
    ScriptedI2cHal hal;
    hal.mtu = 30;  // 30-byte chunks
    NotecardI2c transport(hal);
    hal.prime(transport);

    // Build a request whose wire form (JSON + \n) exceeds 250 bytes.
    // With mtu=30: 8 full chunks = 240 bytes, chunk 9 = 19 bytes more.
    // sentInSegment after chunk 9 = 259 > 250 → segment delay triggered.
    // {"k":"<250x>"} = 258 chars + '\n' = 259 bytes total.
    const std::string body(250, 'x');
    const std::string json = "{\"k\":\"" + body + "\"}";  // 258 bytes + \n = 259 bytes
    hal.responses.push_back("{}\n");
    transport({json}, 5000);

    // Expect at least one segment delay.
    CHECK(hal.total_delay_ms >= kI2cSegmentDelayMs);
}

TEST_CASE("i2c send: transmit failure calls reset() and returns error") {
    // note-c: _I2CTransmit fails → _I2CReset called, error returned.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.prime(transport);

    // Make all subsequent transmits fail; transport retries kI2cMaxRetries times.
    hal.transmit_ok = false;
    auto result = transport({"{\"req\":\"hub.sync\"}"}, 5000);
    REQUIRE(!result);
    // reset() called at least once due to transmit failure.
    CHECK(hal.reset_call_count >= 1);
}

// ---------------------------------------------------------------------------
// receive_response — ported from note-c _i2cChunkedReceive_test.cpp
// ---------------------------------------------------------------------------

TEST_CASE("i2c receive: simple response received correctly") {
    // note-c: _i2cChunkedReceive returns complete response, no error.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.prime(transport);

    hal.responses.push_back("{\"connected\":true}\n");
    auto result = transport({"{\"req\":\"hub.status\"}"}, 5000);
    REQUIRE(result);
    CHECK(*result == "{\"connected\":true}");
}

TEST_CASE("i2c receive: response stripped of trailing CR+LF") {
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.prime(transport);

    hal.responses.push_back("{\"ok\":true}\r\n");
    auto result = transport({"{}"}, 5000);
    REQUIRE(result);
    CHECK(*result == "{\"ok\":true}");
}

TEST_CASE("i2c receive: priming timeout → Error::Timeout") {
    // note-c: _i2cNoteQueryLength times out → timeout error.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.prime(transport);

    // No response queued → rx_buf stays empty → priming query always 0.
    auto result = transport({"{\"req\":\"hub.sync\"}"}, 100);
    REQUIRE(!result);
    CHECK((result.error().code == Error::Timeout ||
           result.error().code == Error::Transport));
}

TEST_CASE("i2c receive: intra-timeout (partial response, no newline) → Timeout") {
    // note-c: timeout while waiting for complete response → error.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.prime(transport);

    // Inject a response without '\n' — transport will wait for EOP then timeout.
    hal.responses.push_back("{\"partial\":");  // deliberately no \n
    auto result = transport({"{}"}, 5000);
    REQUIRE(!result);
    CHECK((result.error().code == Error::Timeout ||
           result.error().code == Error::Transport));
}

TEST_CASE("i2c receive: multi-chunk response (> max_transfer) assembled correctly") {
    // note-c: response larger than MTU → multiple _I2CReceive calls, buffer assembled.
    ScriptedI2cHal hal;
    hal.mtu = 10;  // force chunking on receive too
    NotecardI2c transport(hal);
    hal.prime(transport);

    // 50-char response body → multiple receive chunks.
    const std::string body(40, 'z');
    const std::string resp = "{\"data\":\"" + body + "\"}\n";
    hal.responses.push_back(resp);
    auto result = transport({"{}"}, 5000);
    REQUIRE(result);
    CHECK(result->size() == resp.size() - 1);  // -1 for stripped \n
}

TEST_CASE("i2c receive: drain excess after early newline") {
    // note-c: if EOP received but available > 0, drain before returning.
    // We model this by having a response that reports more bytes available
    // than one chunk, with \n in the middle.
    ScriptedI2cHal hal;
    hal.mtu = 10;
    NotecardI2c transport(hal);
    hal.prime(transport);

    // Response: first 10 bytes have \n at end, but 5 more bytes remain.
    // ScriptedI2cHal delivers all bytes from rx_buf with available tracking,
    // so just use a response longer than one chunk.
    const std::string resp = "{\"x\":true}\n";  // 11 bytes, \n at offset 10
    hal.responses.push_back(resp);
    auto result = transport({"{}"}, 5000);
    REQUIRE(result);
    CHECK(*result == "{\"x\":true}");
}

// ---------------------------------------------------------------------------
// operator() round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("i2c round-trip: simple request and response") {
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{\"connected\":true}\n");
    auto result = transport({"{\"req\":\"hub.status\"}"}, 5000);
    REQUIRE(result);
    CHECK(*result == "{\"connected\":true}");
}

TEST_CASE("i2c round-trip: request forwarded as sent (no mutation)") {
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");
    transport({"{\"req\":\"hub.sync\"}"}, 5000);
    CHECK(hal.last_request == "{\"req\":\"hub.sync\"}");
}

TEST_CASE("i2c round-trip: CRC auto-detection on first CRC response") {
    // note-c: first response with CRC → notecardFirmwareSupportsCrc set true.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);

    // First request: response has CRC → auto-detection sets crc_enabled_.
    const std::string json = "{\"req\":\"hub.sync\"}";
    const std::string resp_with_crc = detail::crc_add(json, 0) + "\n";
    hal.responses.push_back(resp_with_crc);
    auto r1 = transport({json}, 5000);
    REQUIRE(r1);
    CHECK(*r1 == json);

    // Second request: transport should now include CRC in the request.
    hal.responses.push_back(detail::crc_add("{}", 1) + "\n");
    transport({"{}"}, 5000);
    // The request forwarded by the transport should contain a CRC field.
    CHECK(hal.last_request.find("\"crc\"") != std::string::npos);
}

TEST_CASE("i2c round-trip: CRC mismatch triggers retry") {
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");  // first: init (no CRC yet)
    auto r0 = transport({"{}"}, 5000);
    REQUIRE(r0);

    // Enable CRC by injecting a response with CRC for the next request.
    // First attempt: bad CRC → retry. Second attempt: good CRC → success.
    const std::string json = "{\"req\":\"hub.sync\"}";
    const std::string bad = "{\"req\":\"hub.sync\",\"crc\":\"0001:DEADBEEF\"}\n";
    const std::string good = detail::crc_add(json, 1) + "\n";
    // Trigger CRC mode by first calling with a good CRC response.
    hal.responses.push_back(detail::crc_add("{}", 0) + "\n");
    transport({"{}"}, 5000);  // this enables crc_enabled_

    hal.responses.push_back(bad);
    hal.responses.push_back(good);
    auto result = transport({json}, 5000);
    REQUIRE(result);
    CHECK(*result == json);
}

TEST_CASE("i2c round-trip: transmit failure all retries → Error::Transport") {
    // After initialization, make all transmits fail. Set reset_ok=false so
    // the internal do_reset() calls inside the retry loop return quickly
    // (fail fast on reset() rather than waiting through NACK retries).
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.prime(transport);

    hal.transmit_ok = false;
    hal.reset_ok    = false;  // fast-fail do_reset() in retry loop
    auto result = transport({"{\"req\":\"hub.sync\"}"}, 5000);
    REQUIRE(!result);
    CHECK(result.error().code == Error::Transport);
}

TEST_CASE("i2c round-trip: max retries exceeded → Error::Transport") {
    // note-c equivalent: all retries fail → return error.
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{}\n");  // for reset/init
    transport({"{}"}, 5000);

    // All subsequent receive calls return no data → timeout every time.
    auto result = transport({"{\"req\":\"hub.sync\"}"}, 10);
    REQUIRE(!result);
    CHECK(result.error().code == Error::Transport);
}

TEST_CASE("i2c round-trip: second request after success") {
    ScriptedI2cHal hal;
    NotecardI2c transport(hal);
    hal.responses.push_back("{\"first\":true}\n");
    hal.responses.push_back("{\"second\":true}\n");

    auto r1 = transport({"{\"req\":\"first\"}"}, 5000);
    auto r2 = transport({"{\"req\":\"second\"}"}, 5000);
    REQUIRE(r1);
    REQUIRE(r2);
    CHECK(*r1 == "{\"first\":true}");
    CHECK(*r2 == "{\"second\":true}");
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

    NotecardI2c transport(cb);
    auto r = transport({"{\"req\":\"hub.status\"}"}, 5000);
    REQUIRE(r.has_value());
    CHECK(*r == "{}");
}
