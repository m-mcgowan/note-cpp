// Tests for AbstractTransport Phase 2 streaming methods:
//   - set_receive_buffer() + transact() with external buffer
//   - transact_into() — one-shot caller buffer
//   - transact_streaming() — chunk-by-chunk with callback

#include "catch.hpp"

#include <note/transport.hpp>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace note;

// ---------------------------------------------------------------------------
// MockStreamTransport — minimal AbstractTransport for testing Phase 2
//
// Uses a byte queue for do_read(). Transmit is a no-op (always succeeds).
// Reset always succeeds. No CRC. Single retry.
// ---------------------------------------------------------------------------

class MockStreamTransport : public AbstractTransport {
public:
    std::deque<uint8_t> rx;          // bytes returned by do_read()
    std::string last_transmitted;    // last transmit payload
    int transmit_count = 0;
    bool transmit_fails = false;
    bool reset_fails = false;

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
    }

protected:
    bool do_transmit(const char* data, size_t len) override {
        ++transmit_count;
        if (transmit_fails) return false;
        last_transmitted.assign(data, len);
        return true;
    }

    Result<void> do_receive(std::string& buf, uint32_t /*timeout_ms*/) override {
        // Read until \n (simulating buffered receive for the original path)
        while (!rx.empty()) {
            uint8_t b = rx.front();
            rx.pop_front();
            if (b == '\n') {
                // Strip trailing \r
                while (!buf.empty() && buf.back() == '\r') buf.pop_back();
                return {};
            }
            buf.push_back(static_cast<char>(b));
        }
        return make_error(Error::ResponseLost, Cause::Timeout, "timeout");
    }

    bool do_reset() override {
        return !reset_fails;
    }

    Result<size_t> do_read(uint8_t* buf, size_t max_len, uint32_t /*timeout_ms*/) override {
        if (rx.empty())
            return make_error(Error::ResponseLost, Cause::Timeout, "no data");
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        return n;
    }

    uint32_t max_retries() const override { return 1; }
    uint32_t retry_delay_ms() const override { return 0; }
    void delay(uint32_t /*ms*/) override {}
};


// ---------------------------------------------------------------------------
// transact_into() — one-shot receive into caller buffer
// ---------------------------------------------------------------------------

TEST_CASE("transact_into: basic response into caller buffer") {
    MockStreamTransport t;
    t.queue_response("{\"ok\":true}\r\n");

    char buf[128];
    auto r = t.transact_into("{\"req\":\"hub.status\"}", 5000, buf, sizeof(buf));
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"ok\":true}");
    // The string_view points into our buffer
    REQUIRE(r->data() == buf);
}

TEST_CASE("transact_into: strips trailing \\r") {
    MockStreamTransport t;
    t.queue_response("{}\r\r\n");

    char buf[64];
    auto r = t.transact_into("{\"req\":\"test\"}", 5000, buf, sizeof(buf));
    REQUIRE(r.has_value());
    REQUIRE(*r == "{}");
}

TEST_CASE("transact_into: read error triggers retry") {
    MockStreamTransport t;
    // No data queued → do_read fails → retry → still no data → fail
    char buf[64];
    auto r = t.transact_into("{\"req\":\"test\"}", 5000, buf, sizeof(buf));
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == Error::ResponseLost);
}

TEST_CASE("transact_into: transmit failure retries and resets") {
    MockStreamTransport t;
    t.transmit_fails = true;
    t.queue_response("{}\r\n");

    char buf[64];
    auto r = t.transact_into("{\"req\":\"test\"}", 5000, buf, sizeof(buf));
    // Both attempts fail because transmit_fails is permanent
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == Error::SendFailed);
}

TEST_CASE("transact_into: chunked read across multiple do_read calls") {
    // Simulate data arriving in small chunks
    MockStreamTransport t;
    // Queue bytes one at a time to force multiple do_read calls
    std::string response = "{\"v\":1}\r\n";
    for (char c : response) {
        t.rx.push_back(static_cast<uint8_t>(c));
    }

    char buf[64];
    auto r = t.transact_into("{\"req\":\"test\"}", 5000, buf, sizeof(buf));
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"v\":1}");
}


// ---------------------------------------------------------------------------
// set_receive_buffer() + transact() — external buffer path
// ---------------------------------------------------------------------------

TEST_CASE("set_receive_buffer: transact reads into external buffer") {
    MockStreamTransport t;
    t.queue_response("{\"ext\":true}\r\n");

    char ext[128];
    t.set_receive_buffer(ext, sizeof(ext));

    auto r = t.transact("{\"req\":\"hub.status\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"ext\":true}");
    // string_view should point into ext buffer
    REQUIRE(r->data() == ext);
}

TEST_CASE("set_receive_buffer(nullptr): reverts to internal buffer") {
    MockStreamTransport t;
    t.queue_response("{\"first\":1}\r\n");

    char ext[128];
    t.set_receive_buffer(ext, sizeof(ext));
    auto r1 = t.transact("{\"req\":\"first\"}", 5000);
    REQUIRE(r1.has_value());
    REQUIRE(r1->data() == ext);

    // Revert to internal buffer
    t.set_receive_buffer(nullptr, 0);
    t.queue_response("{\"second\":2}\r\n");
    auto r2 = t.transact("{\"req\":\"second\"}", 5000);
    REQUIRE(r2.has_value());
    REQUIRE(*r2 == "{\"second\":2}");
    // Should NOT point into ext
    REQUIRE(r2->data() != ext);
}

TEST_CASE("set_receive_buffer: read error retries") {
    MockStreamTransport t;
    // No data → do_read fails
    char ext[64];
    t.set_receive_buffer(ext, sizeof(ext));
    auto r = t.transact("{\"req\":\"test\"}", 5000);
    REQUIRE(!r.has_value());
}


// ---------------------------------------------------------------------------
// transact_streaming() — chunk-by-chunk with callback
// ---------------------------------------------------------------------------

TEST_CASE("transact_streaming: collects full response via callback") {
    MockStreamTransport t;
    t.queue_response("{\"stream\":true}\r\n");

    uint8_t chunk_buf[8];  // small chunk to exercise multi-read
    std::string collected;

    auto r = t.transact_streaming(
        "{\"req\":\"test\"}", 5000,
        chunk_buf, sizeof(chunk_buf),
        [&](const uint8_t* data, size_t len) {
            collected.append(reinterpret_cast<const char*>(data), len);
        }
    );
    REQUIRE(r.has_value());
    // Callback receives raw data including \r (CRC strip happens after).
    // \n is the frame delimiter and is not passed to the callback.
    REQUIRE(collected == "{\"stream\":true}\r");
}

TEST_CASE("transact_streaming: single-byte chunks") {
    MockStreamTransport t;
    t.queue_response("OK\r\n");

    uint8_t chunk_buf[1];  // 1-byte chunks
    std::vector<std::string> chunks;

    auto r = t.transact_streaming(
        "{\"req\":\"test\"}", 5000,
        chunk_buf, sizeof(chunk_buf),
        [&](const uint8_t* data, size_t len) {
            chunks.emplace_back(reinterpret_cast<const char*>(data), len);
        }
    );
    REQUIRE(r.has_value());
    // Should get individual bytes: 'O', 'K', '\r' (before \n terminates)
    // Actually \r comes before \n, the on_chunk sees it, but CRC strip removes trailing \r
    // The callback is called per-chunk before CRC strip
    REQUIRE(!chunks.empty());
    std::string full;
    for (auto& c : chunks) full += c;
    // Callback sees raw data including \r (CRC strip happens on response_buf_)
    REQUIRE(full.find("OK") != std::string::npos);
}

TEST_CASE("transact_streaming: read error returns failure") {
    MockStreamTransport t;
    // No data queued → do_read fails

    uint8_t chunk_buf[32];
    auto r = t.transact_streaming(
        "{\"req\":\"test\"}", 5000,
        chunk_buf, sizeof(chunk_buf),
        [](const uint8_t*, size_t) {}
    );
    REQUIRE(!r.has_value());
}


// ---------------------------------------------------------------------------
// Original transact() still works (regression)
// ---------------------------------------------------------------------------

TEST_CASE("transact: original buffered path still works") {
    MockStreamTransport t;
    t.queue_response("{\"reg\":true}\r\n");

    // No external buffer set — uses response_buf_
    auto r = t.transact("{\"req\":\"test\"}", 5000);
    REQUIRE(r.has_value());
    REQUIRE(*r == "{\"reg\":true}");
}

TEST_CASE("transact: reset on first use") {
    MockStreamTransport t;
    t.queue_response("{}\r\n");
    auto r = t.transact("{\"req\":\"test\"}", 5000);
    REQUIRE(r.has_value());

    // Second call should not re-reset
    t.queue_response("{}\r\n");
    auto r2 = t.transact("{\"req\":\"test2\"}", 5000);
    REQUIRE(r2.has_value());
}

TEST_CASE("transact: reset failure returns NotReady") {
    MockStreamTransport t;
    t.reset_fails = true;
    auto r = t.transact("{\"req\":\"test\"}", 5000);
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == Error::NotReady);
}
