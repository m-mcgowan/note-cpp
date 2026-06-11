#include "doctest.h"
#include <note/bus_lock.hpp>
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
