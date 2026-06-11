#include "doctest.h"
#include <note/bus_lock.hpp>
#include <note/hal_byte_transport.hpp>
#include <note/protocol.hpp>
#include <note/link/serial.hpp>

#include <deque>
#include <mutex>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// ScriptedSerialHal — minimal scripted HAL for driving Protocol in tests.
// Mirrors the pattern in test_txn_handshake.cpp.
// ---------------------------------------------------------------------------
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

struct RecordingLock : note::IBusLock {
    int locks = 0;
    int unlocks = 0;
    int held = 0;
    int max_held = 0;
    void lock() override   { ++locks; ++held; if (held > max_held) max_held = held; }
    void unlock() override { --held; ++unlocks; }
};

} // namespace

TEST_CASE("BusLockGuard brackets lock()/unlock() around its scope") {
    RecordingLock lk;
    {
        note::BusLockGuard guard{&lk};
        CHECK(lk.locks == 1);
        CHECK(lk.unlocks == 0);
        CHECK(lk.held == 1);
    }
    CHECK(lk.unlocks == 1);
    CHECK(lk.held == 0);
}

TEST_CASE("BusLockGuard with null lock is a no-op") {
    note::BusLockGuard guard{nullptr};
    CHECK(true);
}

TEST_CASE("LockAdapter wraps any C++ lockable") {
    std::mutex m;
    note::LockAdapter<std::mutex> adapter{m};
    note::IBusLock& as_lock = adapter;
    as_lock.lock();
    CHECK(m.try_lock() == false);
    as_lock.unlock();
    CHECK(m.try_lock() == true);
    m.unlock();
}

TEST_CASE("CallbackBusLock forwards to C function pointers") {
    int calls = 0;
    auto lk = [](void* ctx) { (*static_cast<int*>(ctx)) += 1; };
    auto ul = [](void* ctx) { (*static_cast<int*>(ctx)) += 10; };
    note::CallbackBusLock cb{lk, ul, &calls};
    cb.lock();
    cb.unlock();
    CHECK(calls == 11);
}

// ---------------------------------------------------------------------------
// Protocol bus-lock integration: set_bus_lock acquires/releases once per
// exchange.
// ---------------------------------------------------------------------------

TEST_CASE("bus lock: transact acquires and releases once per exchange") {
    ScriptedSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> ns{hal};
    note::Protocol transport{ns};

    RecordingLock lk;
    transport.set_bus_lock(lk);

    hal.queue("{\"connected\":true}\r\n");

    note::JsonSink null_sink;
    auto build = [&](note::JsonBuilder& b) { b.add("req", "hub.status"); };
    auto r = transport.transact(build, null_sink, 5000u);

    REQUIRE(r.has_value());
    CHECK(lk.locks   == 1);
    CHECK(lk.unlocks == 1);
    CHECK(lk.max_held == 1);
}

// ---------------------------------------------------------------------------
// LockObservingSerialHal — a serial HAL variant that defers the response
// bytes until receive() is first called AFTER the line-terminator transmit.
// This forces receive() to fetch bytes independently from transmit(), so the
// test can detect whether the lock is held at the moment bytes are read.
//
// Byte delivery is deliberately split across two phases:
//   Phase 0 (transmit active):  transmit() stores the line-terminator event
//                               but does NOT stage bytes into rx.
//   Phase 1 (first receive()):  bytes are moved from pending_response into rx
//                               so the first receive() call that the protocol
//                               drives actually delivers them.
//
// This mimics real hardware where the response only arrives over the wire
// after transmit is complete — the lock must span both halves.
// ---------------------------------------------------------------------------
namespace {

struct LockObservingSerialHal : public note::link::SerialHal {
    // Set by test before each transaction
    RecordingLock* lock = nullptr;
    std::string pending_response;   // bytes to deliver on first receive()
    bool response_pending = false;  // set when \r\n terminator seen
    bool delivered = false;         // true once pending bytes moved to rx
    std::string rx;
    uint32_t now_ms = 0;

    // Result tracking
    int held_on_first_data_rx = -1; // held count when first real byte read

    void queue(const std::string& rsp) {
        pending_response = rsp;
        response_pending = false;
        delivered = false;
        held_on_first_data_rx = -1;
    }

    bool transmit(const uint8_t* d, size_t n) override {
        // Detect reset probe (\n alone) — drain with empty line
        if (n == 1 && d[0] == '\n') {
            rx += "\r\n";
        }
        // Detect line terminator (\r\n) — arm response but do NOT stage bytes
        else if (n == 2 && d[0] == '\r' && d[1] == '\n') {
            response_pending = true;
        }
        return true;
    }

    size_t receive(uint8_t* buf, size_t max_len) override {
        // On first call after line-terminator, move bytes from pending to rx
        if (response_pending && !delivered) {
            rx += pending_response;
            delivered = true;
            // Record lock state at the moment real response bytes arrive
            if (held_on_first_data_rx < 0)
                held_on_first_data_rx = lock ? lock->held : 0;
        }
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(rx[i]);
        rx.erase(0, n);
        return n;
    }

    uint32_t millis() override { return now_ms; }
    void delay(uint32_t ms) override { now_ms += ms; }
};

} // namespace

// ---------------------------------------------------------------------------
// Regression: string_view+JsonSink transact must hold the bus lock DURING
// receive. Bug: the old implementation called send_raw() (which acquired and
// released the lock) and then called receive_dispatch() with no lock held —
// leaving the receive half of the exchange unprotected.
//
// This test uses LockObservingSerialHal, which defers byte delivery until
// receive() is called. held_on_first_data_rx records the lock->held count
// at the moment the first real response bytes are made available. A value
// of 0 means receive ran with no lock — the bug. The fix gives the overload
// its own BusLockGuard spanning transmit+receive, matching transact_raw.
// ---------------------------------------------------------------------------
TEST_CASE("bus lock: string_view+sink transact holds lock during receive") {
    LockObservingSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> ns{hal};
    note::Protocol transport{ns};

    RecordingLock lk;
    transport.set_bus_lock(lk);
    hal.lock = &lk;

    hal.queue("{\"connected\":true}\r\n");

    note::JsonSink null_sink;
    auto r = transport.transact(note::string_view{"{\"req\":\"hub.status\"}"}, null_sink, 5000u);

    REQUIRE(r.has_value());
    CHECK(lk.locks   == 1);
    CHECK(lk.unlocks == 1);
    CHECK(hal.held_on_first_data_rx == 1);
}

// ---------------------------------------------------------------------------
// No-lock path: transact still succeeds when no lock is registered.
// ---------------------------------------------------------------------------
TEST_CASE("bus lock: transact succeeds with no lock registered") {
    ScriptedSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> ns{hal};
    note::Protocol transport{ns};
    // No set_bus_lock call.

    hal.queue("{\"connected\":true}\r\n");

    note::JsonSink null_sink;
    auto build = [&](note::JsonBuilder& b) { b.add("req", "hub.status"); };
    auto r = transport.transact(build, null_sink, 5000u);

    CHECK(r.has_value());
}

// ---------------------------------------------------------------------------
// clear_bus_lock: after clearing, lock is no longer acquired.
// ---------------------------------------------------------------------------
TEST_CASE("bus lock: clear_bus_lock disengages the lock") {
    ScriptedSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> ns{hal};
    note::Protocol transport{ns};

    RecordingLock lk;
    transport.set_bus_lock(lk);
    transport.clear_bus_lock();

    hal.queue("{\"connected\":true}\r\n");

    note::JsonSink null_sink;
    auto build = [&](note::JsonBuilder& b) { b.add("req", "hub.status"); };
    auto r = transport.transact(build, null_sink, 5000u);

    REQUIRE(r.has_value());
    CHECK(lk.locks == 0);
}

// ---------------------------------------------------------------------------
// HalByteTransportT<HalT, Lock> — compile-time lock template parameter.
// NullLock (the default) must be zero-size. CountingTplLock proves that
// begin_transaction acquires and end_transaction releases exactly once per
// exchange.
// ---------------------------------------------------------------------------

namespace {

struct CountingTplLock {
    static inline int locks = 0;
    static inline int unlocks = 0;
    void lock()   { ++locks; }
    void unlock() { ++unlocks; }
};

// Minimal HAL-shaped stub for HalByteTransportT — non-virtual, satisfies
// exactly the methods HalByteTransportT calls on hal_: transmit, read,
// write_line_terminator, reset (returns bool), millis, delay.
struct StubHal {
    // Staged response for read() — '\n' terminates the frame.
    std::string rx_buf;

    void queue(const std::string& s) { rx_buf = s; }

    bool transmit(const uint8_t* /*data*/, size_t /*len*/) { return true; }

    note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t /*timeout_ms*/) {
        size_t n = std::min(max, rx_buf.size());
        for (size_t i = 0; i < n; ++i)
            buf[i] = static_cast<uint8_t>(rx_buf[i]);
        rx_buf.erase(0, n);
        return n;
    }

    bool write_line_terminator() { return true; }
    bool reset() { return true; }
    uint32_t millis() { return 0; }
    void delay(uint32_t) {}
};

} // namespace

static_assert(std::is_empty_v<note::NullLock>, "NullLock must be zero-size");

TEST_CASE("HalByteTransportT: CountingTplLock is acquired in begin_transaction and released in end_transaction") {
    CountingTplLock::locks   = 0;
    CountingTplLock::unlocks = 0;

    StubHal hal;
    // Queue a minimal '\n'-terminated response so end_transaction doesn't drain.
    hal.queue("\n");

    note::HalByteTransportT<StubHal, CountingTplLock> bt{hal};

    auto r = bt.begin_transaction(1000);
    CHECK(r.has_value());
    CHECK(CountingTplLock::locks == 1);

    bt.end_transaction();
    CHECK(CountingTplLock::unlocks == 1);
}

TEST_CASE("HalByteTransportT: default NullLock compiles and begin/end cycle succeeds") {
    StubHal hal;
    hal.queue("\n");

    // Single-arg instantiation — Lock defaults to NullLock.
    note::HalByteTransportT<StubHal> bt{hal};

    auto r = bt.begin_transaction(1000);
    CHECK(r.has_value());
    bt.end_transaction();
    // NullLock: no observable counters — just prove the path compiles and runs.
    CHECK(true);
}
