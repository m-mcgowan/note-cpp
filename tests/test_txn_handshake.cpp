// Tests for the optional transaction-handshake (CTX/RTX) hook on
// Protocol. Verifies that:
//   - With no TxnHandshake registered, transactions proceed without bracket calls.
//   - begin_operation() calls start() and end_operation() calls stop() (once per operation).
//   - Multiple transact() calls within one begin/end_operation bracket see only
//     ONE start and ONE stop (the whole point of operation-scope RTX).
//   - When start() returns false, begin_operation() returns false and stop() is
//     NOT called.
//   - Notecard-level: one operation with multiple exchanges = one start/stop.

#include <doctest.h>

#include <note/protocol.hpp>
#include <note/link/serial.hpp>
#include <note/txn_handshake.hpp>
#include <note/notecard.hpp>
#include <note/static_notecard.hpp>
#include <note/arena.hpp>

#include <deque>
#include <string>

namespace {

// ScriptedSerialHal — mirrors the pattern in test_transport_serial.cpp.
// Reacts to reset probes (bare '\n') and request terminators ('\r\n') by
// injecting the next queued JSON response.
struct ScriptedSerialHal : public note::link::SerialHal {
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
    note::link::SerialFramer<note::link::SerialPolicy> ns{hal};
    note::Protocol transport{ns};

    // Fire a trivial transact INSIDE a begin/end_operation bracket.
    // Returns the transact Result.
    note::Result<void> transact_in_operation(uint32_t timeout_ms = 5000) {
        transport.begin_operation(timeout_ms);
        note::Protocol& t = transport;
        note::JsonSink null_sink;
        auto build = [&](note::JsonBuilder& b) { b.add("req", "card.status"); };
        auto rv = t.transact(build, null_sink, timeout_ms);
        transport.end_operation();
        return rv;
    }

    // Fire multiple transacts inside ONE begin/end_operation bracket.
    void transact_two_in_one_operation(uint32_t timeout_ms = 5000) {
        transport.begin_operation(timeout_ms);
        note::JsonSink null_sink;
        auto build = [&](note::JsonBuilder& b) { b.add("req", "card.status"); };
        transport.transact(build, null_sink, timeout_ms);
        transport.transact(build, null_sink, timeout_ms);
        transport.end_operation();
    }

    // Fire a send (fire-and-forget) INSIDE a begin/end_operation bracket.
    note::Result<void> send_in_operation() {
        transport.begin_operation(5000);
        note::Protocol& t = transport;
        auto build = [&](note::JsonBuilder& b) { b.add("cmd", "card.restart"); };
        auto rv = t.send(build);
        transport.end_operation();
        return rv;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Without a TxnHandshake registered, nothing changes.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: transact succeeds without a TxnHandshake") {
    Harness h;
    h.hal.queue("{}\r\n");
    h.transport.begin_operation(5000);
    note::JsonSink null_sink;
    auto build = [&](note::JsonBuilder& b) { b.add("req", "card.status"); };
    auto r = h.transport.transact(build, null_sink, 5000);
    h.transport.end_operation();
    REQUIRE(r.has_value());
}

// ---------------------------------------------------------------------------
// begin_operation() calls start() once; end_operation() calls stop() once.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: begin_operation calls start, end_operation calls stop") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);
    h.hal.queue("{}\r\n");

    auto ok = h.transport.begin_operation(/*timeout_ms=*/1234);
    REQUIRE(ok);
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 0);
    CHECK(handshake.last_start_timeout_ms == 1234u);

    note::JsonSink null_sink;
    auto build = [&](note::JsonBuilder& b) { b.add("req", "card.status"); };
    auto r = h.transport.transact(build, null_sink, 1234);
    REQUIRE(r.has_value());
    CHECK(handshake.starts == 1);  // still just one start
    CHECK(handshake.stops  == 0);  // stop not yet called

    h.transport.end_operation();
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 1);  // exactly one stop
}

// ---------------------------------------------------------------------------
// Key proof: multiple transact()s in ONE operation = one start/stop.
// This is the whole point of lifting RTX to operation scope.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: multiple transacts in one operation = one start/stop") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);
    h.hal.queue("{}\r\n");
    h.hal.queue("{}\r\n");

    h.transport.begin_operation(5000);
    note::JsonSink null_sink;
    auto build = [&](note::JsonBuilder& b) { b.add("req", "card.status"); };
    REQUIRE(h.transport.transact(build, null_sink, 5000).has_value());
    REQUIRE(h.transport.transact(build, null_sink, 5000).has_value());
    h.transport.end_operation();

    CHECK(handshake.starts == 1);  // only one RTX assertion for the whole operation
    CHECK(handshake.stops  == 1);  // only one RTX release
}

TEST_CASE("txn handshake: send in operation calls start/stop once") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);

    auto r = h.send_in_operation();
    REQUIRE(r.has_value());
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 1);
}

// ---------------------------------------------------------------------------
// When start() fails, begin_operation() returns false and stop() is NOT called.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: start() failure: begin_operation returns false, stop not called") {
    Harness h;
    CountingHandshake handshake;
    handshake.start_ok = false;
    h.transport.set_handshake(handshake);

    bool ok = h.transport.begin_operation(5000);
    REQUIRE_FALSE(ok);
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 0);  // no release if nothing was acquired

    // end_operation should be a no-op (nothing was started).
    h.transport.end_operation();
    CHECK(handshake.stops == 0);  // still no stop after end_operation
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

    h.transport.begin_operation(5000);
    note::JsonSink null_sink;
    auto build = [&](note::JsonBuilder& b) { b.add("req", "card.status"); };
    auto r = h.transport.transact(build, null_sink, 5000);
    h.transport.end_operation();

    REQUIRE(r.has_value());
    CHECK(handshake.starts == 0);
    CHECK(handshake.stops  == 0);
}

// ---------------------------------------------------------------------------
// Multiple separate operations each call begin/end_operation independently.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: separate operations each get their own start/stop") {
    Harness h;
    CountingHandshake handshake;
    h.transport.set_handshake(handshake);
    h.hal.queue("{}\r\n");
    h.hal.queue("{}\r\n");
    h.hal.queue("{}\r\n");

    // Each call to transact_in_operation() wraps begin/end_operation.
    REQUIRE(h.transact_in_operation().has_value());
    REQUIRE(h.transact_in_operation().has_value());
    REQUIRE(h.transact_in_operation().has_value());

    CHECK(handshake.starts == 3);  // one per operation
    CHECK(handshake.stops  == 3);
}

// ---------------------------------------------------------------------------
// Notecard-level: run_operation drives begin/end_operation once per
// outermost operation, so a multi-exchange op (e.g. two transacts) sees
// only one start/stop on the handshake.
//
// Wire-up: construct a Protocol, set the handshake on it, pass to Notecard.
// ---------------------------------------------------------------------------

namespace {

// MinimalRequest satisfies Notecard::execute()'s RequestT concept.
// void Response so execute_streaming takes the simple path.
struct MinimalRequest {
    static constexpr note::string_view notecard_request = "card.status";
    static constexpr note::Safety safety = note::Safety::NonIdempotent;
    using Response = void;
    void build(note::JsonBuilder&) const {}
};

} // namespace

TEST_CASE("txn handshake: Notecard::execute wraps one start/stop per operation") {
    ScriptedSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> framer{hal};
    note::Protocol transport{framer};

    CountingHandshake handshake;
    transport.set_handshake(handshake);

    hal.queue("{}\r\n");

    note::Notecard nc{transport, note::Allocator{}};

    MinimalRequest req;
    auto rv = nc.execute(req);
    (void)rv;

    // One Notecard::execute = one run_operation = one begin_operation/end_operation.
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 1);
}

// ---------------------------------------------------------------------------
// When begin_operation() returns false (readiness timeout), run_operation
// proceeds anyway and the operation fails at the wire level (not NotReady).
//
// Design trade-off (operation-scope): the old per-exchange TxnHandshakeScope
// short-circuited with NotReady; run_operation does not fast-fail so that
// the outermost caller always gets a concrete transport error back, and the
// retry policy can decide what to do. A readiness timeout now surfaces as a
// wire-level error (ResponseLost/Timeout in this test) rather than NotReady.
// ---------------------------------------------------------------------------

TEST_CASE("txn handshake: begin_operation false => wire-level error, not NotReady") {
    ScriptedSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> framer{hal};
    note::Protocol transport{framer};

    CountingHandshake handshake;
    handshake.start_ok = false;
    transport.set_handshake(handshake);

    // No response queued — when the operation proceeds despite failed readiness,
    // the receive path will time out.
    note::Notecard nc{transport, note::Allocator{}};
    nc.set_default_timeout(1);  // short timeout so the test runs quickly

    MinimalRequest req;
    auto result = nc.execute(req);

    // The result must be an error (not a success).
    REQUIRE_FALSE(result);

    // start() was called once (begin_operation was invoked).
    CHECK(handshake.starts == 1);

    // stop() was NOT called because start() returned false — nothing to release.
    CHECK(handshake.stops == 0);

    // The error is a wire-level transport error (ResponseLost), NOT NotReady.
    // This pins the contract: callers that check for NotReady to detect RTX
    // timeout will NOT see it here; they see the downstream failure instead.
    CHECK(result.error().code != note::Error::NotReady);
    CHECK(result.error().code == note::Error::ResponseLost);
}

TEST_CASE("txn handshake: two Notecard::execute calls = two start/stop pairs") {
    ScriptedSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> framer{hal};
    note::Protocol transport{framer};

    CountingHandshake handshake;
    transport.set_handshake(handshake);

    hal.queue("{}\r\n");
    hal.queue("{}\r\n");

    note::Notecard nc{transport, note::Allocator{}};

    MinimalRequest req;
    nc.execute(req);
    nc.execute(req);

    // Two separate operations = two start/stop pairs.
    CHECK(handshake.starts == 2);
    CHECK(handshake.stops  == 2);
}
