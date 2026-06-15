// Tests for the optional Notecard operation lock.
//
// set_request_lock(IBusLock&) installs a lock that is acquired once for the
// whole operation (request + response) and released on completion.
// The lock is re-entrant at the Notecard level: if a public entry point
// internally calls another entry point (e.g. execute(RequestT&) delegating
// to execute(const RequestT&)), the lock is held exactly once across the
// outer call, not re-acquired on the inner call.
#include "doctest.h"
#include <note/notecard.hpp>
#include "common/scripted_transport.hpp"

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

TEST_CASE("operation lock: re-entrant — execute(T&) -> execute(const T&) acquires once") {
    note::test::ScriptedTransport tx;
    tx.response = R"({})";

    // Use a streaming Notecard (has allocator) so execute_streaming is taken.
    // note::Allocator{} is the default malloc-backed allocator.
    note::Notecard nc{tx, note::Allocator{}};
    RecordingLock lk;
    nc.set_request_lock(lk);

    // execute(RequestT&) → run_operation (depth 0→1, lock acquired) →
    // checks binary traits (none) → execute(const RequestT&) →
    // run_operation (depth 1→2, lock NOT acquired) → execute_streaming →
    // returns → run_operation unwind (depth 2→1, no unlock) →
    // outer run_operation unwind (depth 1→0, lock released).
    MinimalReq req{};
    auto rv = nc.execute(req);
    (void)rv; // void response, just checking the lock counts

    CHECK(lk.locks     == 1);
    CHECK(lk.unlocks   == 1);
    CHECK(lk.max_depth == 1);
    CHECK(lk.depth     == 0);
}
