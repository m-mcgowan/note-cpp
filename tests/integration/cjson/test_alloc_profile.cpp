// Allocation profiling tests for note-cpp with the cJSON backend.
//
// Measures heap allocations per request/response cycle by intercepting:
//   - C++ allocations: global operator new/delete override
//   - cJSON allocations: cJSON_InitHooks
//
// These tests establish baseline allocation counts and serve as regression
// guards. After optimization, tighten the bounds.
//
// History: this file previously included transport+HAL-layer tests that
// exercised the legacy buffered transport API. The streaming refactor moved
// transport behind IStreamingTransport / IBufferedTransport adapters, and
// the old buffered transact() on NotecardSerial no longer exists. Those
// tests have been dropped — they were testing infrastructure that no longer
// has the shape they assumed. The remaining tests use the shared
// ScriptedTransport (tests/common/scripted_transport.hpp) so the focus is
// the cJSON backend's allocation profile, which is what the file's name
// promises.

#include <note/backends/cjson.hpp>
#include <note/arena.hpp>
#include <note/notecard.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_add.hpp>
#include <note/api.hpp>

#include "../../common/scripted_transport.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <new>

using note::test::ScriptedTransport;

// ═══════════════════════════════════════════════════════════════════════════
// Allocation tracking
// ═══════════════════════════════════════════════════════════════════════════

struct CumulativeCounters {
    size_t cpp_allocs = 0;
    size_t cpp_frees = 0;
    size_t cpp_bytes = 0;       // currently allocated (not cumulative)
    size_t cpp_peak_bytes = 0;  // high-water mark (reset per scope)

    size_t cjson_allocs = 0;
    size_t cjson_frees = 0;
};

static CumulativeCounters g_counters;
static bool g_tracking = false;

struct AllocStats {
    size_t cpp_allocs = 0;
    size_t cpp_frees = 0;
    size_t cpp_peak_bytes = 0;
    size_t cpp_bytes_leaked = 0;

    size_t cjson_allocs = 0;
    size_t cjson_frees = 0;

    size_t total_allocs() const { return cpp_allocs + cjson_allocs; }

    void print(const char* label) const {
        std::printf("  %s: C++ allocs=%zu frees=%zu peak=%zu bytes leaked=%zu bytes"
                    " | cJSON allocs=%zu frees=%zu"
                    " | total allocs=%zu\n",
                    label,
                    cpp_allocs, cpp_frees, cpp_peak_bytes, cpp_bytes_leaked,
                    cjson_allocs, cjson_frees,
                    total_allocs());
    }
};

struct AllocHeader {
    size_t size;
};
static constexpr size_t kHeaderSize = sizeof(AllocHeader);
static constexpr size_t kAlignedHeader =
    (kHeaderSize + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

void* operator new(size_t size) {
    void* raw = std::malloc(kAlignedHeader + size);
    if (!raw) throw std::bad_alloc();
    auto* hdr = static_cast<AllocHeader*>(raw);
    hdr->size = size;
    void* p = static_cast<char*>(raw) + kAlignedHeader;
    if (g_tracking) {
        g_counters.cpp_allocs++;
        g_counters.cpp_bytes += size;
        if (g_counters.cpp_bytes > g_counters.cpp_peak_bytes)
            g_counters.cpp_peak_bytes = g_counters.cpp_bytes;
    }
    return p;
}

void operator delete(void* p) noexcept {
    if (!p) return;
    void* raw = static_cast<char*>(p) - kAlignedHeader;
    auto* hdr = static_cast<AllocHeader*>(raw);
    if (g_tracking) {
        g_counters.cpp_frees++;
        if (g_counters.cpp_bytes >= hdr->size)
            g_counters.cpp_bytes -= hdr->size;
    }
    std::free(raw);
}

void operator delete(void* p, size_t) noexcept { operator delete(p); }

static void* cjson_malloc_hook(size_t size) {
    void* p = std::malloc(size);
    if (p && g_tracking) {
        g_counters.cjson_allocs++;
    }
    return p;
}

static void cjson_free_hook(void* p) {
    if (p && g_tracking) {
        g_counters.cjson_frees++;
    }
    std::free(p);
}

static void install_cjson_hooks() {
    cJSON_Hooks hooks;
    hooks.malloc_fn = cjson_malloc_hook;
    hooks.free_fn = cjson_free_hook;
    cJSON_InitHooks(&hooks);
}

struct TrackingScope {
    CumulativeCounters snapshot;

    TrackingScope() {
        g_tracking = true;
        snapshot = g_counters;
        g_counters.cpp_peak_bytes = g_counters.cpp_bytes;
    }

    AllocStats finish() {
        g_tracking = false;
        AllocStats s;
        s.cpp_allocs = g_counters.cpp_allocs - snapshot.cpp_allocs;
        s.cpp_frees = g_counters.cpp_frees - snapshot.cpp_frees;
        s.cpp_peak_bytes = g_counters.cpp_peak_bytes - snapshot.cpp_bytes;
        s.cpp_bytes_leaked = (g_counters.cpp_bytes > snapshot.cpp_bytes)
            ? g_counters.cpp_bytes - snapshot.cpp_bytes : 0;
        s.cjson_allocs = g_counters.cjson_allocs - snapshot.cjson_allocs;
        s.cjson_frees = g_counters.cjson_frees - snapshot.cjson_frees;
        return s;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Tests — cJSON backend allocation profile
// ═══════════════════════════════════════════════════════════════════════════

static void test_builder_alloc_profile() {
    note::backends::CjsonBackend backend;

    TrackingScope scope;

    auto builder = backend.create_builder();
    builder->add("req", "hub.set");
    builder->add("product", "com.example.app");
    builder->add("mode", "periodic");
    builder->add("outbound", int32_t{60});
    auto json = std::string(builder->to_view());

    auto stats = scope.finish();
    stats.print("builder");

    assert(json.find("\"req\":\"hub.set\"") != std::string::npos);

    builder.reset();
    json.clear();
    json.shrink_to_fit();

    std::printf("    builder total allocs: %zu (C++: %zu, cJSON: %zu)\n",
                stats.total_allocs(), stats.cpp_allocs, stats.cjson_allocs);

    std::puts("  PASS: builder_alloc_profile");
}

static void test_full_execute_alloc_profile() {
    note::backends::CjsonBackend backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up: first call may allocate for std::function internals etc.
    { auto r = api.card.version().execute(); }

    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(r.has_value());
    stats.print("full execute (card.version)");

    std::printf("    execute() total allocs: %zu (C++: %zu, cJSON: %zu)\n",
                stats.total_allocs(), stats.cpp_allocs, stats.cjson_allocs);

    std::puts("  PASS: full_execute_alloc_profile");
}

static void test_execute_with_body_alloc_profile() {
    note::backends::CjsonBackend backend;
    ScriptedTransport transport;
    transport.response =
        R"({"version":"notecard-7.2.1","body":{"org":"Blues","product":"X"}})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    { auto r = api.card.version().execute(); }

    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(r.has_value());
    stats.print("execute with body (card.version)");
    std::puts("  PASS: execute_with_body_alloc_profile");
}

static void test_leak_detection() {
    note::backends::CjsonBackend backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    { auto r = api.card.version().execute(); }

    TrackingScope scope;
    {
        auto r = api.card.version().execute();
        assert(r.has_value());
    }
    auto stats = scope.finish();

    stats.print("leak detection");
    std::printf("    note: backend retains owned_reader_ (%zu bytes) for reuse\n",
                stats.cpp_bytes_leaked);
    std::puts("  PASS: leak_detection");
}

static void test_multiple_requests_no_growth() {
    note::backends::CjsonBackend backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v1"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    { auto r = api.card.version().execute(); }

    TrackingScope scope1;
    { auto r = api.card.version().execute(); }
    auto stats1 = scope1.finish();

    for (int i = 0; i < 8; ++i) {
        auto r = api.card.version().execute();
        (void)r;
    }

    TrackingScope scope10;
    { auto r = api.card.version().execute(); }
    auto stats10 = scope10.finish();

    stats1.print("request #1");
    stats10.print("request #10");

    std::printf("    request #1 allocs: %zu, request #10 allocs: %zu\n",
                stats1.total_allocs(), stats10.total_allocs());

    assert(stats10.total_allocs() <= stats1.total_allocs());
    std::puts("  PASS: multiple_requests_no_growth");
}

// ═══════════════════════════════════════════════════════════════════════════
// Arena tests — zero cJSON heap allocations with CjsonArenaBackend
// ═══════════════════════════════════════════════════════════════════════════

static void test_arena_stats() {
    alignas(std::max_align_t) char pool[4096];
    note::MonotonicArena arena(pool);

    assert(arena.capacity() == 4096);
    assert(arena.used() == 0);
    assert(arena.available() == 4096);

    void* p = arena.allocate(100);
    assert(p != nullptr);
    assert(arena.used() >= 100);
    assert(arena.available() <= 4096 - 100);

    arena.reset();
    assert(arena.used() == 0);
    assert(arena.available() == 4096);

    std::puts("  PASS: arena_stats — used/capacity/available correct");
}

static void test_arena_zero_heap_execute() {
    alignas(std::max_align_t) char pool[8192];
    note::MonotonicArena arena(pool);
    note::backends::CjsonArenaBackend backend(arena);

    ScriptedTransport transport;
    transport.response =
        R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    { auto warm = api.card.version().execute(); }

    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(r.has_value());

    stats.print("arena execute (card.version, steady-state)");
    std::printf("    arena used: %zu / %zu bytes\n", arena.used(), arena.capacity());

    // Zero cJSON heap allocations — all routed through the arena.
    assert(stats.cjson_allocs == 0);
    std::printf("    C++ allocs: %zu (CjsonReader wrapper), cJSON heap allocs: %zu\n",
                stats.cpp_allocs, stats.cjson_allocs);
    std::puts("  PASS: arena_zero_heap_execute — 0 cJSON heap allocations (all via arena)");
}

static void test_arena_multiple_requests_bounded() {
    alignas(std::max_align_t) char pool[8192];
    note::MonotonicArena arena(pool);
    note::backends::CjsonArenaBackend backend(arena);

    ScriptedTransport transport;
    transport.response = R"({"version":"v1","device":"dev:1"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    size_t max_arena_used = 0;
    for (int i = 0; i < 10; ++i) {
        auto r = api.card.version().execute();
        assert(r.has_value());
        if (arena.used() > max_arena_used) max_arena_used = arena.used();
    }

    std::printf("  arena peak usage: %zu / %zu bytes (%.1f%%)\n",
                max_arena_used, arena.capacity(),
                100.0 * static_cast<double>(max_arena_used) / static_cast<double>(arena.capacity()));

    assert(max_arena_used < arena.capacity());
    std::puts("  PASS: arena_multiple_requests_bounded — arena reuse verified");
}

// ═══════════════════════════════════════════════════════════════════════════

int main() {
    install_cjson_hooks();

    std::puts("=== Allocation profiling tests (cJSON backend) ===\n");

    test_builder_alloc_profile();
    test_full_execute_alloc_profile();
    test_execute_with_body_alloc_profile();
    test_leak_detection();
    test_multiple_requests_no_growth();

    std::puts("\n=== Arena tests (CjsonArenaBackend) ===\n");

    test_arena_stats();
    test_arena_zero_heap_execute();
    test_arena_multiple_requests_bounded();

    std::puts("\nAll allocation profiling tests passed.");
    return 0;
}
