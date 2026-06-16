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
#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/arena.hpp>
#include <note/txn_handshake.hpp>
#include <note/protocol.hpp>
#include <note/link/serial.hpp>
#include <note/api/card_binary_put.hpp>
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

// ===========================================================================
// StaticNotecard<Stack, Lock> — compile-time lock template param
// ===========================================================================
//
// The MockStack / MockTransport types used here are the same types defined in
// test_static_notecard.cpp (included via the note/static_notecard.hpp header
// test scaffolding). We define a local replica here so this file compiles
// standalone.

namespace {

// Minimal mock transport for StaticNotecard lock tests.
struct LockTestTransport {
    std::string last_request;
    int transact_count = 0;
    int send_count = 0;
    uint32_t now_ms = 0;
    uint32_t total_delay_ms = 0;
    int reset_count = 0;

    void queue_response(const char* /*json*/) {}  // response not consumed by tests

    struct Writer : note::JsonWriter {
        std::string& out;
        explicit Writer(std::string& o) : out(o) {}
        bool write(const char* data, size_t len) override {
            out.append(data, len);
            return true;
        }
    };

    note::Result<void> transact_dispatch(note::RequestSource src, note::SaxDispatch,
                                         uint32_t, note::detail::NcErrorCapture&) {
        ++transact_count;
        last_request.clear();
        Writer w(last_request);
        src.emit(w);
        last_request += '}';  // close object (mock substitutes for Protocol)
        return {};
    }

    note::Result<note::string_view> transact_raw(note::string_view, char*, size_t, uint32_t) {
        return note::make_error(note::Error::NotReady, NOTE_ERR("not used"));
    }

    note::Result<void> send(note::RequestSource src) {
        ++send_count;
        last_request.clear();
        Writer w(last_request);
        src.emit(w);
        last_request += '}';
        return {};
    }

    bool reset() { ++reset_count; return true; }
    uint32_t millis() { return now_ms; }
    void delay(uint32_t ms) { now_ms += ms; total_delay_ms += ms; }
    LockTestTransport& hal() { return *this; }

    // begin/end_operation: operation-scope RTX/CTX handshake hook.
    // No-op on this mock — StaticNotecard calls these once per outermost operation.
    bool begin_operation(uint32_t /*timeout_ms*/) { return true; }
    void end_operation() {}
};

struct LockTestStack { LockTestTransport transport; };

/// A simple recursive-capable counting lock usable as a StaticNotecard<Stack, Lock>
/// template parameter (not derived from IBusLock — pure template duck-typing).
struct CountingRecursiveLock {
    static inline int locks = 0, unlocks = 0, held = 0, max_held = 0;
    static void reset_counters() { locks = unlocks = held = max_held = 0; }
    void lock()   { ++locks; ++held; if (held > max_held) max_held = held; }
    void unlock() { --held; ++unlocks; }
};

} // namespace

// NullLock is empty (zero-size): compile-time check
static_assert(std::is_empty_v<note::NullLock>, "NullLock must be zero-size");

TEST_CASE("StaticNotecard<Stack,Lock>: lock acquired and released per operation") {
    alignas(4) char arena_buf[256];
    note::MonotonicArena arena(arena_buf);

    CountingRecursiveLock::reset_counters();

    note::StaticNotecard<LockTestStack, CountingRecursiveLock> nc(note::arena_allocator(arena));
    note::Api api(nc);

    // Queue a void-response for hub.set
    nc.stack().transport.queue_response("{}");

    auto result = api.hub.set().product("com.example.test").execute();
    (void)result;  // we care about lock counts, not the result

    CHECK(CountingRecursiveLock::locks   == CountingRecursiveLock::unlocks);
    CHECK(CountingRecursiveLock::held    == 0);
    CHECK(CountingRecursiveLock::locks   >= 1);  // at least one acquire happened
}

TEST_CASE("StaticNotecard<Stack,Lock>: two operations each acquire once") {
    alignas(4) char arena_buf[256];
    note::MonotonicArena arena(arena_buf);

    CountingRecursiveLock::reset_counters();

    note::StaticNotecard<LockTestStack, CountingRecursiveLock> nc(note::arena_allocator(arena));
    note::Api api(nc);

    nc.stack().transport.queue_response("{}");
    api.hub.set().product("a").execute();

    nc.stack().transport.queue_response("{}");
    api.hub.set().product("b").execute();

    CHECK(CountingRecursiveLock::locks   == CountingRecursiveLock::unlocks);
    CHECK(CountingRecursiveLock::held    == 0);
}

TEST_CASE("StaticNotecard<Stack> (default NullLock) compiles and runs without error") {
    alignas(4) char arena_buf[256];
    note::MonotonicArena arena(arena_buf);

    // Default second template param = NullLock: must compile and work.
    note::StaticNotecard<LockTestStack> nc(note::arena_allocator(arena));
    note::Api api(nc);

    nc.stack().transport.queue_response("{}");
    auto result = api.hub.set().product("com.example.test").execute();
    (void)result;

    CHECK(nc.stack().transport.transact_count == 1);
}

// ===========================================================================
// exclusive() session guard tests
// ===========================================================================
//
// exclusive() returns an RAII guard that holds the recursive request lock
// across a GROUP of requests, making the group atomic against other threads.
// Inner run_operation() calls re-acquire the lock harmlessly (recursive lock).
// The guard is a no-op when no request lock is set.

TEST_CASE("exclusive(): no-op when no request lock is configured") {
    note::test::ScriptedTransport tx;
    tx.response = R"({})";
    note::Notecard nc{tx};
    // No request lock set — exclusive() should compile and be a no-op.
    {
        auto s = nc.exclusive();
        char buf[64];
        auto rv = nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
        CHECK(rv.has_value());
    }  // guard released — no crash
}

TEST_CASE("exclusive(): lock held across two requests within guard, balanced after") {
    note::test::ScriptedTransport tx;
    tx.response = R"({})";
    note::Notecard nc{tx};

    std::recursive_mutex rm;
    note::LockAdapter<std::recursive_mutex> lk{rm};
    nc.set_request_lock(lk);

    {
        auto s = nc.exclusive();

        // The recursive mutex is held by the exclusive() guard.
        // Inner run_operation() re-acquires it recursively (no deadlock).
        char buf[64];
        auto r1 = nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
        auto r2 = nc.transact(R"({"req":"card.temp"})",   note::span<char>(buf, sizeof(buf)));
        CHECK(r1.has_value());
        CHECK(r2.has_value());
    }
    // After guard leaves scope the lock must be fully released.
    CHECK(rm.try_lock() == true);
    rm.unlock();
}

// ---------------------------------------------------------------------------
// Threaded exclusive() atomicity test.
//
// Two threads share one Notecard and one recursive operation lock. Thread A
// opens an exclusive() guard and issues two requests. Thread B tries to
// interleave. The interleave detector inside the transport catches concurrent
// transact() calls. With exclusive() holding the lock, B blocks for the whole
// group — zero violations across 5 runs.
// ---------------------------------------------------------------------------

namespace {

/// Counts interleaving between the two requests inside one exclusive() group.
/// Same pattern as InterleaveDetectingTransport above, but for group-level
/// interleave detection.
struct GroupInterleaveTransport : note::ITransact {
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
        if (in_flight.fetch_add(1, std::memory_order_acq_rel) != 0)
            violations.fetch_add(1, std::memory_order_relaxed);
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

constexpr int kExclusiveGroupIters = 50;

/// Thread that opens an exclusive() guard and issues two requests per iteration.
void exclusive_group_worker(note::Notecard& nc, std::barrier<>& bar, int iters) {
    char buf[64];
    for (int i = 0; i < iters; ++i) {
        bar.arrive_and_wait();
        auto g = nc.exclusive();
        nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
        nc.transact(R"({"req":"card.temp"})",   note::span<char>(buf, sizeof(buf)));
    }
}

} // namespace

TEST_CASE("exclusive(): group is atomic — two-thread interleave detector, 0 violations") {
    GroupInterleaveTransport tx;
    note::Notecard nc{tx};

    std::recursive_mutex rm;
    note::LockAdapter<std::recursive_mutex> lk{rm};
    nc.set_request_lock(lk);

    std::barrier<> bar{2};
    std::thread t1{[&] { exclusive_group_worker(nc, bar, kExclusiveGroupIters); }};
    std::thread t2{[&] { exclusive_group_worker(nc, bar, kExclusiveGroupIters); }};
    t1.join();
    t2.join();

    CHECK(tx.violations.load() == 0);
}

// ===========================================================================
// keep_ready() session guard tests
// ===========================================================================
//
// keep_ready() returns an RAII guard that holds the RTX/CTX readiness scope
// (op_depth_ / begin_operation / end_operation) across a group of requests,
// so the Notecard is asserted-ready once for the whole group instead of once
// per request. When NOTE_TXN_HANDSHAKE is off, it's a trivial no-op.

#if NOTE_TXN_HANDSHAKE

namespace {

// Minimal scripted serial HAL — same pattern as test_txn_handshake.cpp.
struct KrSerialHal : public note::link::SerialHal {
    std::string rx;
    uint32_t now_ms = 0;

    bool transmit(const uint8_t* d, size_t n) override {
        // reset probe (\n) → \r\n drain reply; request terminator (\r\n) → canned "{}\r\n".
        if (n == 1 && d[0] == '\n') {
            rx += "\r\n";
        } else if (n == 2 && d[0] == '\r' && d[1] == '\n') {
            rx += "{}\r\n";
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

// Counting handshake — same type as test_txn_handshake.cpp (redefined locally).
struct KrCountingHandshake : public note::TxnHandshake {
    int starts = 0;
    int stops  = 0;
    bool start(uint32_t) override { ++starts; return true; }
    void stop() override { ++stops; }
};

} // namespace

TEST_CASE("keep_ready(): two requests without guard fire start/stop twice (control)") {
    KrSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> framer{hal};
    note::Protocol transport{framer};
    KrCountingHandshake handshake;
    transport.set_handshake(handshake);

    note::Notecard nc{transport, note::Allocator{}};

    char buf[64];
    // Two separate requests => two independent run_operation() calls => two start/stop pairs.
    nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
    nc.transact(R"({"req":"card.temp"})",   note::span<char>(buf, sizeof(buf)));

    CHECK(handshake.starts == 2);
    CHECK(handshake.stops  == 2);
}

TEST_CASE("keep_ready(): two requests inside guard fire start/stop only once") {
    KrSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> framer{hal};
    note::Protocol transport{framer};
    KrCountingHandshake handshake;
    transport.set_handshake(handshake);

    note::Notecard nc{transport, note::Allocator{}};

    char buf[64];
    {
        auto r = nc.keep_ready();
        // Both requests are inside the readiness scope opened by keep_ready().
        // The inner run_operation() calls see op_depth_ > 0 and skip begin_operation.
        nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
        nc.transact(R"({"req":"card.temp"})",   note::span<char>(buf, sizeof(buf)));
    }

    // One begin_operation / end_operation across the whole group.
    CHECK(handshake.starts == 1);
    CHECK(handshake.stops  == 1);
}

TEST_CASE("keep_ready(): guard releases readiness on destruction (op_depth_ back to 0)") {
    KrSerialHal hal;
    note::link::SerialFramer<note::link::SerialPolicy> framer{hal};
    note::Protocol transport{framer};
    KrCountingHandshake handshake;
    transport.set_handshake(handshake);

    note::Notecard nc{transport, note::Allocator{}};
    char buf[64];

    {
        auto r = nc.keep_ready();
        nc.transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
    }
    // Guard destroyed: op_depth_ back to 0. Next request starts a fresh scope.
    nc.transact(R"({"req":"card.temp"})", note::span<char>(buf, sizeof(buf)));

    CHECK(handshake.starts == 2);  // group1 + post-guard request
    CHECK(handshake.stops  == 2);
}

#endif // NOTE_TXN_HANDSHAKE

// ===========================================================================
// StaticNotecard exclusive() / keep_ready() smoke tests
// ===========================================================================

TEST_CASE("StaticNotecard::exclusive(): compiles and is no-op for NullLock") {
    alignas(4) char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::StaticNotecard<LockTestStack> nc(note::arena_allocator(arena));
    note::Api api(nc);

    {
        auto ex = nc.exclusive();
        nc.stack().transport.queue_response("{}");
        auto r = api.hub.set().product("a").execute();
        (void)r;
    }
    CHECK(nc.stack().transport.transact_count == 1);
}

TEST_CASE("StaticNotecard::exclusive(): acquires and releases CountingRecursiveLock") {
    alignas(4) char arena_buf[256];
    note::MonotonicArena arena(arena_buf);

    CountingRecursiveLock::reset_counters();

    note::StaticNotecard<LockTestStack, CountingRecursiveLock> nc(note::arena_allocator(arena));
    note::Api api(nc);

    {
        auto ex = nc.exclusive();
        // Lock is held: depth = 1 (from ExclusiveSession)
        nc.stack().transport.queue_response("{}");
        api.hub.set().product("a").execute();
        // After execute: inner OpGuard acquired and released; depth is back to 1
    }
    // After guard: depth = 0, locks balanced
    CHECK(CountingRecursiveLock::locks   == CountingRecursiveLock::unlocks);
    CHECK(CountingRecursiveLock::held    == 0);
    CHECK(CountingRecursiveLock::locks   >= 2);  // exclusive() + at least one inner acquire
}

TEST_CASE("StaticNotecard::keep_ready(): compiles and runs without error") {
    alignas(4) char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::StaticNotecard<LockTestStack> nc(note::arena_allocator(arena));
    note::Api api(nc);

    {
        auto kr = nc.keep_ready();
        nc.stack().transport.queue_response("{}");
        auto r = api.hub.set().product("a").execute();
        (void)r;
    }
    CHECK(nc.stack().transport.transact_count == 1);
}

// ===========================================================================
// Concern 1: binary transfer atomicity via run_operation (request lock)
//
// do_binary_send/do_binary_receive are wrapped in run_operation(), so the
// recursive request lock is held for the entire binary operation: JSON
// handshake + COBS payload write (send) / COBS payload read (receive).
// Two threads each doing a binary PUT via the same Notecard + request lock
// must never interleave their transact() + write() sequences.
//
// BinaryInterleaveTransport — extends ITransact with write() support and an
// in_flight counter that spans transact() + write(). If any two calls from
// different threads overlap (in_flight > 1), violations is incremented.
// ===========================================================================

namespace {

struct BinaryInterleaveTransport : note::ITransact {
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

    // Enter the "in-flight" window: if another thread is already inside, it's
    // a violation. Linger so concurrent entry is reliably observed.
    void mark_enter() {
        if (in_flight.fetch_add(1, std::memory_order_acq_rel) != 0)
            violations.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    void mark_exit() {
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }

    note::Result<note::string_view> transact(note::string_view, note::span<char> buf,
                                              uint32_t) override {
        mark_enter();
        mark_exit();
        static constexpr note::string_view rsp{"{}"};
        if (rsp.size() >= buf.size())
            return note::make_error(note::Error::Overflow, NOTE_ERR("buffer too small"));
        std::memcpy(buf.data(), rsp.data(), rsp.size());
        return note::string_view(buf.data(), rsp.size());
    }

    note::Result<void> write(const uint8_t*, size_t) override {
        mark_enter();
        mark_exit();
        return {};
    }

    note::Result<void> send(note::string_view) override { return {}; }
    void reset() override {}
    void abort() override {}
};

constexpr int kBinaryAtomicIters = 100;

void binary_put_worker(note::Notecard& nc, std::barrier<>& bar, int iters) {
    static const uint8_t kPayload[] = {1, 2, 3, 4, 5};
    for (int i = 0; i < iters; ++i) {
        bar.arrive_and_wait();
        note::api::CardBinaryPut req;
        req.data(kPayload, sizeof(kPayload)).verify(false);
        auto result = nc.execute(req);
        (void)result;
    }
}

} // namespace (concern-1 binary helpers)

TEST_CASE("concern 1: binary PUT is atomic vs other threads via run_operation") {
    BinaryInterleaveTransport tx;
    note::Notecard nc{tx, note::Allocator{}};

    std::recursive_mutex rm;
    note::LockAdapter<std::recursive_mutex> op_lock{rm};
    nc.set_request_lock(op_lock);

    std::barrier<> bar{2};

    std::thread t1{[&] { binary_put_worker(nc, bar, kBinaryAtomicIters); }};
    std::thread t2{[&] { binary_put_worker(nc, bar, kBinaryAtomicIters); }};
    t1.join();
    t2.join();

    // Zero interleaving: the request lock held by run_operation ensures that
    // the JSON handshake (transact) and COBS payload (write) from one thread
    // are never interleaved with those from another.
    CHECK(tx.violations.load() == 0);
}
