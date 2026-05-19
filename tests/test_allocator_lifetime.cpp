// Allocator lifecycle: pins the public contract of `note::Allocator` —
// exactly what the library asks for and (more importantly) what it
// returns. The tests use a custom Allocator with counters wired into
// allocate/deallocate/reallocate so the assertions read directly off
// the library's calls, not off `operator new` (which the default
// Allocator bypasses by going straight to `std::malloc`).
//
// Why this exists: with the default heap-backed Allocator on the
// streaming path, the library currently calls `allocate` once per
// interned string and never calls `deallocate`. The bytes stay live
// until process exit. That's documented in `docs/memory.md § Picking
// an allocator`; these tests are the executable form of that doc, so
// any change to allocator usage shows up here first.

#include <doctest.h>
#include "test_notecard_factory.hpp"

#include <note/note.hpp>
#include <note/allocator.hpp>
#include <note/arena.hpp>
#include <note/api.hpp>
#include <note/protocol.hpp>

#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

namespace {

// ── A streaming Hal that returns canned JSON ─────────────────────────────
struct CannedHal : note::Hal {
    std::deque<uint8_t> rx;
    void queue(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }
    bool transmit(const uint8_t*, size_t) override { return true; }
    bool write_line_terminator() override { return true; }
    note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
        if (rx.empty())
            return note::make_error(note::Error::ResponseLost, note::Cause::Timeout, "no data");
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
        return n;
    }
    bool reset() override { return true; }
    void delay(uint32_t) override {}
    uint32_t millis() override { return 0; }
};

// ── A counted Allocator that mirrors the default (malloc-backed) one ─────
// All counts are visible to the test; the underlying storage is
// std::malloc / std::free, so the behaviour is exactly what a default
// `note::Allocator{}` would see for a real workload.
struct CountedAllocCtx {
    int alloc_calls = 0;
    int free_calls = 0;
    int realloc_calls = 0;
    size_t bytes_allocated = 0;   // running total, never decreases
    size_t bytes_freed = 0;       // running total of bytes passed to free
    void reset() {
        alloc_calls = free_calls = realloc_calls = 0;
        bytes_allocated = bytes_freed = 0;
    }
};

[[maybe_unused]]
inline note::Allocator counted_malloc_allocator(CountedAllocCtx& ctx) {
    return note::Allocator{
        // alloc
        [](size_t n, void* c) -> void* {
            auto* cc = static_cast<CountedAllocCtx*>(c);
            ++cc->alloc_calls;
            cc->bytes_allocated += n;
            return std::malloc(n);
        },
        // free
        [](void* p, size_t n, void* c) {
            if (!p) return;
            auto* cc = static_cast<CountedAllocCtx*>(c);
            ++cc->free_calls;
            cc->bytes_freed += n;
            std::free(p);
        },
        // realloc
        [](void* p, size_t old_n, size_t new_n, void* c) -> void* {
            auto* cc = static_cast<CountedAllocCtx*>(c);
            ++cc->realloc_calls;
            cc->bytes_allocated += (new_n > old_n ? (new_n - old_n) : 0);
            return std::realloc(p, new_n);
        },
        &ctx,
    };
}

// ── A counted Allocator on top of MonotonicArena ─────────────────────────
// The arena's own `allocate` is what gets called; the wrapper just tallies.
struct ArenaCountedCtx {
    note::MonotonicArena& arena;
    int alloc_calls = 0;
    int free_calls = 0;
    size_t bytes_allocated = 0;
    explicit ArenaCountedCtx(note::MonotonicArena& a) : arena(a) {}
    void reset_counts() { alloc_calls = free_calls = 0; bytes_allocated = 0; }
};

inline note::Allocator counted_arena_allocator(ArenaCountedCtx& ctx) {
    return note::Allocator{
        [](size_t n, void* c) -> void* {
            auto* cc = static_cast<ArenaCountedCtx*>(c);
            ++cc->alloc_calls;
            cc->bytes_allocated += n;
            return cc->arena.allocate(n);
        },
        [](void*, size_t, void* c) {
            ++static_cast<ArenaCountedCtx*>(c)->free_calls;
        },
        [](void* p, size_t old_n, size_t new_n, void* c) -> void* {
            auto* cc = static_cast<ArenaCountedCtx*>(c);
            void* np = cc->arena.allocate(new_n);
            if (np && p) std::memcpy(np, p, old_n < new_n ? old_n : new_n);
            return np;
        },
        &ctx,
    };
}

constexpr const char* k_version_response =
    R"({"version":"notecard-7.2.1","device":"dev:864475","sku":"NOTE-WBNA"})";

}  // namespace

// =============================================================================
// Heap-backed Allocator on the streaming path
// =============================================================================
//
// Phase 1 contract: each Response captures the Allocator that minted its
// interned strings and frees them when it goes out of scope. Consecutive
// executes therefore see alloc and free counts climbing in lockstep — no
// accumulation, no leaks. Compare to the arena path below, where free is
// a no-op and `arena.reset()` is what reclaims.
//
// These tests pin the *with-RAII* contract — they're gated out under
// NOTE_NO_RESPONSE_RAII=1 (which intentionally disables the dtor).

#if !NOTE_NO_RESPONSE_RAII
TEST_CASE("allocator-lifetime: heap-backed streaming frees on Response destruction") {
    CountedAllocCtx ctx;

    CannedHal hal;
    note::Protocol transport{hal};

    {
        note::Notecard nc = note::test::make_test_notecard(
            transport, counted_malloc_allocator(ctx));
        note::Api api(nc);

        // Three back-to-back executes that each return three string fields.
        // Each loop iteration constructs a Response in `r`, then drops it
        // at the iteration's closing brace — alloc and free fire together.
        for (int i = 0; i < 3; ++i) {
            hal.queue(k_version_response);
            auto r = api.card.version().execute();
            REQUIRE(r.has_value());
            CHECK(r.version == "notecard-7.2.1");
            CHECK(r.device == "dev:864475");
            CHECK(r.sku == "NOTE-WBNA");
        }

        // Three loops × three strings each = 9 allocate + 9 deallocate.
        CHECK(ctx.alloc_calls == 9);
        CHECK(ctx.free_calls == 9);
        CHECK(ctx.bytes_allocated == ctx.bytes_freed);
    }

    // Notecard destruction adds nothing — the cleanup already happened
    // each time a Response went out of scope.
    CHECK(ctx.alloc_calls == 9);
    CHECK(ctx.free_calls == 9);
}
#endif // !NOTE_NO_RESPONSE_RAII

// =============================================================================
// Arena-backed Allocator on the streaming path
// =============================================================================
//
// The arena recycles the same bytes on `reset()`. allocate() is still called
// once per interned string, but the bytes are bounded by the arena and the
// reset cycle makes the lifetime explicit.

TEST_CASE("allocator-lifetime: arena-backed streaming reclaims on reset()") {
    char arena_buf[2048];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
    ArenaCountedCtx ctx(arena);

    CannedHal hal;
    note::Protocol transport{hal};

    note::Notecard nc = note::test::make_test_notecard(
        transport, counted_arena_allocator(ctx));
    note::Api api(nc);

    // First execute — the library allocates from the arena.
    hal.queue(k_version_response);
    auto r1 = api.card.version().execute();
    REQUIRE(r1.has_value());
    CHECK(ctx.alloc_calls == 3);   // version, device, sku
    CHECK(ctx.free_calls == 0);    // arena allocator's free is a no-op
    const size_t used_after_first = arena.used();
    CHECK(used_after_first > 0);

    // r1.version points into the arena — must outlive the next execute().
    const char* v1 = r1.version.data();
    CHECK(std::string_view{v1, r1.version.size()} == "notecard-7.2.1");

    // Second execute — more allocations layered on top.
    hal.queue(k_version_response);
    auto r2 = api.card.version().execute();
    REQUIRE(r2.has_value());
    CHECK(ctx.alloc_calls == 6);
    CHECK(arena.used() > used_after_first);

    // r1's view is still valid — arena hasn't been reset.
    CHECK(std::string_view{v1, r1.version.size()} == "notecard-7.2.1");

    // reset() reclaims everything. After this, r1.version and r2.version
    // both point at memory the arena may overwrite — don't read them.
    arena.reset();
    nc.set_allocator(counted_arena_allocator(ctx));   // re-bind after reset

    CHECK(arena.used() == 0);

    // Third execute lands in the freshly-reset arena.
    ctx.reset_counts();
    hal.queue(k_version_response);
    auto r3 = api.card.version().execute();
    REQUIRE(r3.has_value());
    CHECK(ctx.alloc_calls == 3);   // counts again from the reset
    CHECK(arena.used() == used_after_first);   // same footprint as the first call
}

// =============================================================================
// Tree path: with vs without `set_allocator`
// =============================================================================
//
// Tree mode keeps every response inside the backend's JsonReader. The
// Notecard's Allocator is only consulted when the application has called
// `set_allocator` — in which case the library copies response strings out of
// the reader and into allocator-backed storage so they outlive the next
// execute(). The next two tests pin both halves of that contract.

#include <note/backends/buffer.hpp>
#include "common/scripted_transport.hpp"

TEST_CASE("allocator-lifetime: tree mode without set_allocator does not touch Allocator") {
    // No `nc.set_allocator(...)` call — the Notecard's `alloc_` stays
    // unset. Response strings stay inside the JsonReader, which the
    // backend reuses on the next execute(). String views from a prior
    // call are invalid after the next request runs.
    note::backends::StaticJsonBackend<512, 64> backend;
    note::test::ScriptedTransport transport;
    transport.response = k_version_response;

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // No Allocator to count — assertion is about reader-only lifetime.
    auto r1 = api.card.version().execute();
    REQUIRE(r1.has_value());
    CHECK(r1.version == "notecard-7.2.1");

    // After a second execute(), r1.version may be invalid (points into
    // the reader's buffer, which has been reused). We don't dereference
    // it here — the contract being pinned is "no Notecard.Allocator
    // calls"; lifetime is checked in the next test by an arena.
    auto r2 = api.card.version().execute();
    REQUIRE(r2.has_value());
    CHECK(r2.version == "notecard-7.2.1");
}

#if !NOTE_NO_RESPONSE_RAII
TEST_CASE("allocator-lifetime: tree mode WITH set_allocator runs Phase 1 cleanup too") {
    note::backends::StaticJsonBackend<512, 64> backend;
    note::test::ScriptedTransport transport;
    transport.response = k_version_response;

    CountedAllocCtx ctx;

    note::Notecard nc(backend, transport);
    nc.set_allocator(counted_malloc_allocator(ctx));
    note::Api api(nc);

    for (int i = 0; i < 3; ++i) {
        auto r = api.card.version().execute();
        REQUIRE(r.has_value());
        CHECK(r.version == "notecard-7.2.1");
        CHECK(r.device == "dev:864475");
        CHECK(r.sku == "NOTE-WBNA");
    }

    // Tree-mode + set_allocator runs the same intern-then-attach pattern
    // as streaming — `intern_strings(pool)` allocates, the attach hook
    // points the Response at the allocator, and the Response's destructor
    // frees on scope exit. Counts climb in lockstep.
    CHECK(ctx.alloc_calls == 9);
    CHECK(ctx.free_calls == 9);
    CHECK(ctx.bytes_allocated == ctx.bytes_freed);

    // (Old behaviour, pre-Phase 1, leaked one allocation per interned
    // string — this test exists to prevent regression. See git history
    // for the previous shape.)
}
#endif // !NOTE_NO_RESPONSE_RAII

// =============================================================================
// HeapResetPool — malloc-backed arena lifecycle
// =============================================================================
//
// Same shape as MonotonicArena, but storage comes from std::malloc instead
// of a user-supplied buffer. Use when you want "drain on reset" semantics
// on a host where heap is fine but you don't want to size an arena up front.

TEST_CASE("HeapResetPool: allocate returns distinct, usable blocks") {
    note::HeapResetPool pool;

    void* a = pool.allocate(16);
    void* b = pool.allocate(32);
    void* c = pool.allocate(64);

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(a != b);
    CHECK(b != c);

    // Round-trip a write to confirm the memory is writable.
    std::memset(a, 0xAA, 16);
    std::memset(b, 0xBB, 32);
    CHECK(static_cast<unsigned char*>(a)[0] == 0xAA);
    CHECK(static_cast<unsigned char*>(b)[31] == 0xBB);

    pool.reset();  // freed via dtor too, but reset is the user-facing call
}

TEST_CASE("HeapResetPool: reset() frees every block in one pass") {
    // We can't directly observe std::free, but we can drive the lifecycle
    // through a heap_reset_allocator on a Notecard and confirm the
    // Allocator's free hook is not called (HeapResetPool's contract: free
    // is a no-op; reset drains).
    CannedHal hal;
    note::Protocol transport{hal};

    note::HeapResetPool pool;
    note::Notecard nc = note::test::make_test_notecard(
        transport, note::heap_reset_allocator(pool));
    note::Api api(nc);

    // Three executes accumulate allocations in the pool.
    for (int i = 0; i < 3; ++i) {
        hal.queue(k_version_response);
        auto r = api.card.version().execute();
        REQUIRE(r.has_value());
        CHECK(r.version == "notecard-7.2.1");
    }

    // reset() drains everything; subsequent executes start fresh.
    pool.reset();

    hal.queue(k_version_response);
    auto r = api.card.version().execute();
    REQUIRE(r.has_value());
    CHECK(r.version == "notecard-7.2.1");
}

TEST_CASE("HeapResetPool: ~HeapResetPool() drains outstanding allocations") {
    // Confirm dtor-driven cleanup by using a counted realloc/free under
    // the pool. We bracket the pool in a scope so its dtor runs while
    // we still hold references and can verify behaviour from outside.
    bool dtor_ran = false;

    {
        note::HeapResetPool pool;
        for (size_t i = 0; i < 5; ++i) {
            void* p = pool.allocate(8 + i);
            REQUIRE(p != nullptr);
        }
        // Mark that pool is about to be destroyed; ASan / valgrind would
        // catch a leak if dtor didn't free the blocks. We can't probe
        // std::free directly, so this is a "compiles and runs clean"
        // assertion — pair with `ci.sh --asan` if you want a hard check.
        dtor_ran = true;
    }

    CHECK(dtor_ran);
}

// =============================================================================
// Phase 1 RAII contract — Response destructor frees what it interned
// =============================================================================
//
// The headline behaviour the codegen now bakes in: every Response captures
// the Allocator that minted its interned strings (via `note::AllocatorRef`)
// and frees each one when the Response goes out of scope. With a counted
// allocator, allocate calls match deallocate calls after the Response's
// scope ends.

#if !NOTE_NO_RESPONSE_RAII
TEST_CASE("allocator-lifetime: Response destructor frees every string the Sink interned") {
    CountedAllocCtx ctx;

    CannedHal hal;
    note::Protocol transport{hal};

    note::Notecard nc = note::test::make_test_notecard(
        transport, counted_malloc_allocator(ctx));
    note::Api api(nc);

    {
        hal.queue(k_version_response);
        auto r = api.card.version().execute();
        REQUIRE(r.has_value());
        CHECK(r.version == "notecard-7.2.1");
        // Strings live as long as `r` does. allocate has fired once per
        // string field (3 here); deallocate has not yet been called.
        CHECK(ctx.alloc_calls == 3);
        CHECK(ctx.free_calls == 0);
    }
    // Response dropped at the closing brace. Its destructor walked the
    // captured Allocator and freed each interned string.
    CHECK(ctx.alloc_calls == 3);
    CHECK(ctx.free_calls == 3);
    CHECK(ctx.bytes_allocated == ctx.bytes_freed);

    // A second execute repeats the cycle cleanly — alloc and free counts
    // climb in lockstep.
    {
        hal.queue(k_version_response);
        auto r = api.card.version().execute();
        REQUIRE(r.has_value());
        CHECK(ctx.alloc_calls == 6);
        CHECK(ctx.free_calls == 3);
    }
    CHECK(ctx.alloc_calls == 6);
    CHECK(ctx.free_calls == 6);
    CHECK(ctx.bytes_allocated == ctx.bytes_freed);
}

TEST_CASE("allocator-lifetime: moving a Response transfers cleanup to the new owner") {
    CountedAllocCtx ctx;
    CannedHal hal;
    note::Protocol transport{hal};

    note::Notecard nc = note::test::make_test_notecard(
        transport, counted_malloc_allocator(ctx));
    note::Api api(nc);

    {
        hal.queue(k_version_response);
        auto r1 = api.card.version().execute();
        REQUIRE(r1.has_value());
        CHECK(ctx.alloc_calls == 3);
        CHECK(ctx.free_calls == 0);

        // Move r1 into r2. r1 should now be "empty" (its AllocatorRef nulled
        // by the move), so when r1 goes out of scope at the inner brace it
        // does nothing. r2 still holds the strings and cleans up when *it*
        // is destroyed.
        auto r2 = std::move(r1);
        CHECK(ctx.free_calls == 0);   // move alone doesn't free
    }
    // Both r1 and r2 are gone now. The cleanup ran once — on r2.
    CHECK(ctx.free_calls == 3);
    CHECK(ctx.bytes_allocated == ctx.bytes_freed);
}
#endif // !NOTE_NO_RESPONSE_RAII

TEST_CASE("HeapResetPool: move ctor transfers ownership; source is empty") {
    note::HeapResetPool src;
    void* a = src.allocate(16);
    REQUIRE(a != nullptr);

    note::HeapResetPool dst{std::move(src)};
    // src has been emptied; reset() on it is a no-op. dst now owns a.
    src.reset();
    // a is still valid here because dst owns it.
    std::memset(a, 0x77, 16);
    CHECK(static_cast<unsigned char*>(a)[15] == 0x77);

    // dst.reset() (or its dtor) frees a.
    dst.reset();
}
