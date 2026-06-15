// Tests for the optional Notecard operation lock.
//
// set_request_lock(IBusLock&) installs a lock that is acquired once for the
// whole operation (request + response) and released on completion.
// The lock must be a recursive lock: nested entry points (e.g. execute()
// called from within do_binary_send) re-acquire it on the same thread;
// a non-recursive lock would deadlock. Other threads block until the
// outermost operation releases it.
#include "doctest.h"
#include <note/notecard.hpp>
#include "common/scripted_transport.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

struct RecordingLock : note::IBusLock {
    int locks   = 0;
    int unlocks = 0;
    int depth   = 0;
    int max_depth = 0;
    void lock() override   { ++locks;   ++depth; if (depth > max_depth) max_depth = depth; }
    void unlock() override { --depth;   ++unlocks; }
};

} // namespace

TEST_CASE("operation lock: one acquire/release per simple operation") {
    note::test::ScriptedTransport tx;
    tx.response = R"({})";
    note::Notecard nc{tx};
    RecordingLock lk;
    nc.set_request_lock(lk);

    // Drive one operation via the raw JSON transact path — the
    // simplest entry point ScriptedTransport supports.
    char buf[64];
    auto rv = nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
    CHECK(rv.has_value());
    CHECK(lk.locks     == 1);
    CHECK(lk.unlocks   == 1);
    CHECK(lk.max_depth == 1);
    CHECK(lk.depth     == 0);
}

TEST_CASE("operation lock: second operation re-acquires") {
    note::test::ScriptedTransport tx;
    tx.response = R"({})";
    note::Notecard nc{tx};
    RecordingLock lk;
    nc.set_request_lock(lk);

    char buf[64];
    nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
    nc.transact(R"({"req":"card.temp"})",   note::span<char>(buf, sizeof(buf)));

    CHECK(lk.locks     == 2);
    CHECK(lk.unlocks   == 2);
    CHECK(lk.max_depth == 1);
    CHECK(lk.depth     == 0);
}

TEST_CASE("operation lock: clear_request_lock disables locking") {
    note::test::ScriptedTransport tx;
    tx.response = R"({})";
    note::Notecard nc{tx};
    RecordingLock lk;
    nc.set_request_lock(lk);
    nc.clear_request_lock();

    char buf[64];
    nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));

    CHECK(lk.locks   == 0);
    CHECK(lk.unlocks == 0);
}

TEST_CASE("operation lock: no lock = no behavioral change") {
    // Confirm the code path still functions correctly when no lock is set.
    note::test::ScriptedTransport tx;
    tx.response = R"({})";
    note::Notecard nc{tx};

    char buf[64];
    auto rv = nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
    CHECK(rv.has_value());
}

// ── Re-entrancy test ────────────────────────────────────────────────────────
//
// execute(RequestT&) (mutable overload) checks for attached binary buffers;
// finding none, it delegates to execute(const RequestT&). Both overloads are
// wrapped in run_operation(). The depth counter in run_operation must detect
// the nested call (op_depth_ > 0) and skip the inner lock acquisition so the
// lock is held only once across the full call, not twice.
//
// MinimalReq satisfies the minimum RequestT concept for execute(): it has
// notecard_request, safety, a void Response, build(), and the binary-source/
// destination trait checks return false (no binary buffers attached).
namespace {

struct MinimalReq {
    static constexpr note::string_view notecard_request = "card.status";
    static constexpr note::Safety safety = note::Safety::NonIdempotent;
    using Response = void;

    // Required by execute_streaming (the streaming path used when alloc
    // is set). We use a streaming Notecard so this path fires.
    void build(note::JsonBuilder&) const {}

    // Satisfy has_binary_src / has_binary_dst: the request has no
    // binary_src_ or binary_dst_ members, so both detail traits
    // evaluate to false_type — execute(RequestT&) falls straight
    // through to execute(const RequestT&). No explicit no-binary
    // annotation needed.
};

} // namespace

TEST_CASE("operation lock: re-entrant — execute(T&) -> execute(const T&) does not deadlock") {
    note::test::ScriptedTransport tx;
    tx.response = R"({})";

    // Use a streaming Notecard (has allocator) so execute_streaming is taken.
    // note::Allocator{} is the default malloc-backed allocator.
    note::Notecard nc{tx, note::Allocator{}};

    // The operation lock MUST be recursive: execute(RequestT&) wraps
    // run_operation which acquires the lock, then delegates to
    // execute(const RequestT&) which calls run_operation again (re-acquire
    // on the same thread). A non-recursive lock would deadlock here.
    std::recursive_mutex rm;
    note::LockAdapter<std::recursive_mutex> lk{rm};
    nc.set_request_lock(lk);

    // If this returns without deadlocking, the recursive re-acquire works.
    MinimalReq req{};
    auto rv = nc.execute(req);
    (void)rv; // void response, just checking we complete without deadlock

    // With a recursive lock, every lock() is matched by an unlock().
    // We can't observe the count directly, but we can prove the mutex
    // is fully released (try_lock succeeds after the operation).
    CHECK(rm.try_lock() == true);
    rm.unlock();
}

// ---------------------------------------------------------------------------
// Threaded serialization regression: two threads share ONE Notecard and ONE
// recursive operation lock. An interleave detector inside the ITransact stub
// catches any concurrent transact() calls. With the buggy run_operation
// (depth-first lock), thread B skips the lock and races thread A. With the
// fix (lock-first), B blocks until A is done — zero violations.
// ---------------------------------------------------------------------------

namespace {

// InterleaveDetectingTransport — ITransact stub that records concurrent entry
// into transact(). Increments in_flight on entry; if already > 0, another
// thread is simultaneously inside — a violation. Lingers briefly so concurrent
// entry is reliably observed. Returns a canned "{}" response.
struct InterleaveDetectingTransport : note::ITransact {
    std::atomic<int> in_flight{0};
    std::atomic<int> violations{0};

    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return size_t{0}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    } hal_;
    note::Hal& hal() override { return hal_; }

    using note::ITransact::transact;
    using note::ITransact::send;

    note::Result<note::string_view> transact(note::string_view, note::span<char> buf, uint32_t) override {
        // Detect concurrent entry: if in_flight was already > 0, another
        // thread is inside this function — that's a serialization violation.
        if (in_flight.fetch_add(1, std::memory_order_acq_rel) != 0)
            violations.fetch_add(1, std::memory_order_relaxed);

        // Linger so concurrent entry by the other thread is reliably observed.
        std::this_thread::sleep_for(std::chrono::microseconds(100));

        in_flight.fetch_sub(1, std::memory_order_acq_rel);

        static constexpr note::string_view rsp{"{}"};
        if (rsp.size() >= buf.size())
            return note::make_error(note::Error::Overflow, NOTE_ERR("buffer too small"));
        std::memcpy(buf.data(), rsp.data(), rsp.size());
        return note::string_view(buf.data(), rsp.size());
    }

    note::Result<void> send(note::string_view) override { return {}; }
    void reset() override {}
    void abort() override {}
};

constexpr int kSerializeIterations = 200;

void notecard_worker(note::Notecard& nc, std::barrier<>& bar, int iters) {
    char buf[64];
    for (int i = 0; i < iters; ++i) {
        bar.arrive_and_wait();  // synchronize both threads before each operation
        nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
    }
}

} // namespace

TEST_CASE("operation lock: two threads sharing one Notecard serialize operations") {
    InterleaveDetectingTransport tx;
    note::Notecard nc{tx};

    std::recursive_mutex rm;
    note::LockAdapter<std::recursive_mutex> op_lock{rm};
    nc.set_request_lock(op_lock);

    std::barrier bar{2};

    std::thread t1{[&] { notecard_worker(nc, bar, kSerializeIterations); }};
    std::thread t2{[&] { notecard_worker(nc, bar, kSerializeIterations); }};
    t1.join();
    t2.join();

    CHECK(tx.violations.load() == 0);
}
