// SAX-path zero-allocation profiling tests for note-cpp.
//
// Proves that execute() with a configured Allocator (SAX path) performs
// ZERO heap allocations — all response strings are interned into the arena.
// This complements test_alloc_profile.cpp which tests the tree-parse path.

#include <note/backends/buffer.hpp>
#include <note/notecard.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════
// Allocation tracking (same pattern as test_alloc_profile.cpp)
// ═══════════════════════════════════════════════════════════════════════════

static size_t g_alloc_count = 0;
static size_t g_free_count = 0;
static size_t g_bytes_allocated = 0;
static size_t g_peak_bytes = 0;
static bool g_tracking = false;

struct AllocHeader { size_t size; };
static constexpr size_t kAlignedHeader =
    (sizeof(AllocHeader) + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

void* operator new(size_t size) {
    void* raw = std::malloc(kAlignedHeader + size);
    if (!raw) throw std::bad_alloc();
    auto* hdr = static_cast<AllocHeader*>(raw);
    hdr->size = size;
    void* p = static_cast<char*>(raw) + kAlignedHeader;
    if (g_tracking) {
        g_alloc_count++;
        g_bytes_allocated += size;
        if (g_bytes_allocated > g_peak_bytes)
            g_peak_bytes = g_bytes_allocated;
    }
    return p;
}

void operator delete(void* p) noexcept {
    if (!p) return;
    void* raw = static_cast<char*>(p) - kAlignedHeader;
    auto* hdr = static_cast<AllocHeader*>(raw);
    if (g_tracking) {
        g_free_count++;
        if (g_bytes_allocated >= hdr->size)
            g_bytes_allocated -= hdr->size;
    }
    std::free(raw);
}

void operator delete(void* p, size_t) noexcept { operator delete(p); }

struct TrackingScope {
    size_t start_allocs;
    size_t start_frees;
    size_t start_bytes;

    TrackingScope() {
        g_tracking = true;
        start_allocs = g_alloc_count;
        start_frees = g_free_count;
        start_bytes = g_bytes_allocated;
        g_peak_bytes = g_bytes_allocated;
    }

    struct Stats {
        size_t allocs;
        size_t frees;
        size_t peak_bytes;
        size_t leaked_bytes;

        void print(const char* label) const {
            std::printf("  %s: allocs=%zu frees=%zu peak=%zu bytes leaked=%zu bytes\n",
                        label, allocs, frees, peak_bytes, leaked_bytes);
        }
    };

    Stats finish() {
        g_tracking = false;
        Stats s;
        s.allocs = g_alloc_count - start_allocs;
        s.frees = g_free_count - start_frees;
        s.peak_bytes = g_peak_bytes - start_bytes;
        s.leaked_bytes = (g_bytes_allocated > start_bytes)
            ? g_bytes_allocated - start_bytes : 0;
        return s;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Scripted transport — returns string_view into member buffer
// ═══════════════════════════════════════════════════════════════════════════

struct ScriptedTransport : note::ITransport {
    const char* response = "{}";

    note::Result<note::string_view> transact(note::string_view, uint32_t) override {
        return note::string_view(response);
    }
    note::Result<void> send(note::string_view) override { return {}; }
    void reset() override {}
    void abort() override {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════

static void test_sax_zero_alloc_card_version() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0","cell":true})";

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up: first call may allocate for std::function internals
    { auto r = api.card.version().execute(); arena.reset(); }

    // Steady-state: ZERO heap allocations expected
    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(r.has_value());
    assert(r.version == "notecard-7.2.1");
    assert(r.device == "dev:12345");
    assert(r.board == "1.0");
    assert(r.cell == true);

    stats.print("SAX card.version");
    assert(stats.allocs == 0);
    std::puts("  PASS: sax_zero_alloc_card_version — 0 heap allocations");
}

static void test_sax_string_survives_reuse() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    char arena_buf[2048];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.hub.set().execute(); }

    // First request: save version string
    transport.response = R"({"version":"firmware-1.0","device":"dev:AAA"})";
    auto r1 = api.card.version().execute();
    assert(r1.has_value());

    // Save copies of the string data pointers before second request
    const char* v1_data = r1.version.data();
    const char* d1_data = r1.device.data();

    // Second request: transport buffer overwritten
    transport.response = R"({"version":"firmware-2.0","device":"dev:BBB"})";
    auto r2 = api.card.version().execute();
    assert(r2.has_value());

    // r1's strings survive because they're in the arena, not the transport buffer
    assert(r1.version == "firmware-1.0");
    assert(r1.device == "dev:AAA");
    assert(r1.version.data() == v1_data);  // same arena pointer
    assert(r1.device.data() == d1_data);

    // r2 has its own arena-backed copies
    assert(r2.version == "firmware-2.0");
    assert(r2.device == "dev:BBB");

    std::puts("  PASS: sax_string_survives_reuse — strings survive transport buffer reuse");
}

static void test_sax_error_detection() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.hub.set().execute(); }

    // Error response
    transport.response = R"({"err":"file not found"})";
    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(!r.has_value());
    assert(r.error().code == note::Error::Notecard);
    assert(r.error().message == "file not found");

    stats.print("SAX error detection");
    assert(stats.allocs == 0);  // error string interned in arena, not heap
    std::puts("  PASS: sax_error_detection — error interned in arena, 0 heap allocations");
}

static void test_sax_json_parse_error() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.hub.set().execute(); }

    // Malformed JSON
    transport.response = R"({invalid json)";
    auto r = api.card.version().execute();

    assert(!r.has_value());
    assert(r.error().code == note::Error::Json);

    std::puts("  PASS: sax_json_parse_error — malformed JSON returns Error::Json");
}

static void test_sax_bounded_memory() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0"})";

    char arena_buf[4096];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));
    note::Api api(nc);

    // Warm up
    { auto r = api.card.version().execute(); arena.reset(); }

    // Measure arena usage for one request
    size_t used_after_one = 0;
    {
        auto r = api.card.version().execute();
        used_after_one = arena.used();
        arena.reset();
    }

    // Do N requests with arena reset between each — peak memory should be stable
    nc.set_allocator(note::arena_allocator(arena));
    for (int i = 0; i < 100; ++i) {
        auto r = api.card.version().execute();
        assert(r.has_value());
        assert(arena.used() == used_after_one);
        arena.reset();
        nc.set_allocator(note::arena_allocator(arena));
    }

    std::printf("  arena usage per request: %zu bytes\n", used_after_one);
    std::puts("  PASS: sax_bounded_memory — stable arena usage across 100 requests");
}

static void test_sax_via_api() {
    // Full Api → Notecard flow with stored allocator
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v3.5.1","device":"dev:XYZ","name":"notecard"})";

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    note::Notecard nc(backend, transport);
    nc.set_allocator(note::arena_allocator(arena));

    note::Api api(nc);

    // Warm up
    { api.card.version().execute(); arena.reset(); nc.set_allocator(note::arena_allocator(arena)); }

    TrackingScope scope;
    auto r = api.execute(api.card.version());
    auto stats = scope.finish();

    assert(r.has_value());
    assert(r.version == "v3.5.1");
    assert(r.device == "dev:XYZ");
    assert(r.name == "notecard");

    stats.print("SAX via Api");
    assert(stats.allocs == 0);
    std::puts("  PASS: sax_via_api — Api delegates to SAX path, 0 heap allocations");
}

static void test_sax_explicit_allocator() {
    // Per-call allocator overload (no stored allocator on Notecard)
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v1.0","device":"dev:001"})";

    note::Notecard nc(backend, transport);
    // No set_allocator() — using explicit per-call overload

    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));

    // Warm up (tree path, since no allocator configured)
    { nc.execute(note::api::CardVersion{}); }

    TrackingScope scope;
    auto r = nc.execute(note::api::CardVersion{}, note::arena_allocator(arena));
    auto stats = scope.finish();

    assert(r.has_value());
    assert(r.version == "v1.0");
    assert(r.device == "dev:001");

    stats.print("SAX explicit allocator");
    assert(stats.allocs == 0);
    std::puts("  PASS: sax_explicit_allocator — per-call allocator, 0 heap allocations");
}

// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::puts("=== SAX zero-allocation profiling tests ===\n");

    test_sax_zero_alloc_card_version();
    test_sax_string_survives_reuse();
    test_sax_error_detection();
    test_sax_json_parse_error();
    test_sax_bounded_memory();
    test_sax_via_api();
    test_sax_explicit_allocator();

    std::puts("\nAll SAX zero-allocation tests passed.");
    std::puts("SAX path + MonotonicArena + StringPool = ZERO heap allocations.");
    return 0;
}
