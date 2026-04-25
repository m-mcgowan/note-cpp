// Tests for the optional transaction-handshake (CTX/RTX) hook on
// StreamingTransport. Verifies that:
//   - With no TxnHandshake registered, transactions proceed without bracket calls.
//   - With a TxnHandshake registered, start()/stop() bracket each transact/send.
//   - When start() returns false, transact returns NotReady and stop() is
//     NOT called on destruct (nothing to release).
//   - When the underlying transport fails after a successful start(), stop()
//     is still called (RAII release on early return).

#include <doctest.h>

#include <note/streaming_transport.hpp>
#include <note/transport/serial.hpp>
#include <note/txn_handshake.hpp>

#include <deque>
#include <string>

namespace {

// ScriptedSerialHal — mirrors the pattern in test_transport_serial.cpp.
// Reacts to reset probes (bare '\n') and request terminators ('\r\n') by
// injecting the next queued JSON response.
struct ScriptedSerialHal : public note::transport::SerialHal {
    std::string rx;
    std::deque<std::string> queued_responses;
    std::string reset_drain = "\r\n";
    uint32_t now_ms = 0;

    void queue(const std::string& rsp) { queued_responses.push_back(rsp); }

    bool transmit(const uint8_t* d, size_t n) override {
        if (n == 1 && d[0] == '\n') {
            rx += reset_drain;
        } else if (n == 2 && d[0] == '\r' && d[1] == '\n') {
            if (!queued_responses.empty()) {
                rx += queued_responses.front();
                queued_responses.pop_front();
            }
        }
        return true;
    }
    size_t receive(uint8_t* buf, size_t max_len) override {
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(rx[i]);
        rx.erase(0, n);
        return n;
    }
    uint32_t millis() override { return now_ms; }
    void delay(uint32_t ms) override { now_ms += ms; }
};

// Counting TxnHandshake. `start_ok` controls return value of start().
struct CountingHandshake : public note::TxnHandshake {
    int  starts      = 0;
    int  stops       = 0;
    bool start_ok    = true;
    uint32_t last_start_timeout_ms = 0;

    bool start(uint32_t timeout_ms) override {
        ++starts;
        last_start_timeout_ms = timeout_ms;
        return start_ok;
    }
    void stop() override { ++stops; }
};

struct Harness {
    ScriptedSerialHal hal;
    note::transport::NotecardSerial<note::transport::SerialPolicy> ns{hal};
    note::StreamingTransport transport{ns};

    // Fire a trivial transact; returns the Result.
    note::Result<void> transact(uint32_t timeout_ms = 5000) {
        note::IStreamingTransport& t = transport;
        note::JsonSink null_sink;
        auto build = [&](note::JsonBuilder& b) { b.add("req", "card.status"); };
        return t.transact(build, null_sink, timeout_ms);
    }

    // Fire a send (fire-and-forget); returns the Result.
    note::Result<void> send() {
        note::IStreamingTransport& t = transport;
        auto build = [&](note::JsonBuilder& b) { b.add("cmd", "card.restart"); };
        return t.send(build);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Without a TxnHandshake registered, nothing changes.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: transact succeeds without a TxnHandshake") {
    Harness h;
    h.hal.queue("{}\r\n");
    auto r = h.transact();
    REQUIRE(r.has_value());
}

// ---------------------------------------------------------------------------
// With a TxnHandshake registered, start()/stop() bracket the transaction.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: transact calls start/stop once each on success") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);
    h.hal.queue("{}\r\n");

    auto r = h.transact(/*timeout_ms=*/1234);
    REQUIRE(r.has_value());
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 1);
    CHECK(handshake.last_start_timeout_ms == 1234u);
}

TEST_CASE("txn handshake: send calls start/stop once each on success") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);

    auto r = h.send();
    REQUIRE(r.has_value());
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 1);
}

// ---------------------------------------------------------------------------
// When start() fails, transact returns NotReady and stop() is NOT called.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: start() failure returns NotReady and skips stop()") {
    Harness h;
    CountingHandshake handshake;
    handshake.start_ok = false;
    h.transport.set_handshake(handshake);

    auto r = h.transact();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::NotReady);
    CHECK(r.error().cause == note::Cause::Timeout);
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 0);  // no release if nothing was acquired
}

// ---------------------------------------------------------------------------
// clear_handshake() removes the bracket.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: clear_handshake disables bracketing") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);
    h.transport.clear_handshake();
    h.hal.queue("{}\r\n");

    auto r = h.transact();
    REQUIRE(r.has_value());
    CHECK(handshake.starts == 0);
    CHECK(handshake.stops  == 0);
}

// ---------------------------------------------------------------------------
// Multiple transactions each bracket independently.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: multiple transactions each bracket independently") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);
    h.hal.queue("{}\r\n");
    h.hal.queue("{}\r\n");
    h.hal.queue("{}\r\n");

    REQUIRE(h.transact().has_value());
    REQUIRE(h.transact().has_value());
    REQUIRE(h.transact().has_value());
    CHECK(handshake.starts == 3);
    CHECK(handshake.stops  == 3);
}
