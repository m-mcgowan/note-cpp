// Zero-allocation profiling tests for note-cpp with the buffer backend.
//
// Proves that a full execute() cycle (build request → transport → parse
// response → populate Response) requires ZERO heap allocations in steady
// state when using BufferJsonBackend + member-buffer transport.
//
// The operator new/delete overrides count all C++ heap allocations.
// After a warm-up call (which establishes transport buffer capacity),
// subsequent execute() calls must allocate nothing.

#include <note/backends/buffer.hpp>
#include <note/notecard.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_add.hpp>
#include <note/api.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════
// Allocation tracking (same pattern as cjson/test_alloc_profile.cpp)
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
    // Use const char* to avoid any std::string allocation during assignment.
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

static void test_zero_alloc_card_version() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up: first call may allocate for std::function internals
    { auto r = api.card.version().execute(); }

    // Steady-state: ZERO allocations expected
    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(r.has_value());
    assert(r.version == "notecard-7.2.1");
    assert(r.device == "dev:12345");
    assert(r.board == "1.0");

    stats.print("card.version (steady-state)");
    assert(stats.allocs == 0);
    std::puts("  PASS: zero_alloc_card_version — 0 heap allocations");
}

static void test_zero_alloc_hub_set() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    { api.hub.set().mode("periodic").outbound(int32_t{60}).execute(); }

    TrackingScope scope;
    auto r = api.hub.set().mode("periodic").outbound(int32_t{60}).execute();
    auto stats = scope.finish();

    assert(r.has_value());
    stats.print("hub.set (steady-state)");
    assert(stats.allocs == 0);
    std::puts("  PASS: zero_alloc_hub_set — 0 heap allocations");
}

static void test_zero_alloc_multiple_requests() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    static constexpr const char* version_rsp = R"({"version":"v1","device":"dev:1"})";
    static constexpr const char* empty_rsp = R"({})";

    // Warm up with different request types
    transport.response = version_rsp;
    { api.card.version().execute(); }
    transport.response = empty_rsp;
    { api.hub.set().mode("periodic").execute(); }

    // Now measure 10 consecutive requests of mixed types.
    // Transport response is a const char* — zero allocation on assignment.
    TrackingScope scope;
    for (int i = 0; i < 5; ++i) {
        transport.response = version_rsp;
        auto r1 = api.card.version().execute();
        assert(r1.has_value());

        transport.response = empty_rsp;
        auto r2 = api.hub.set().mode("continuous").execute();
        assert(r2.has_value());
    }
    auto stats = scope.finish();

    stats.print("10 mixed requests");
    assert(stats.allocs == 0);
    std::puts("  PASS: zero_alloc_multiple_requests — 10 requests, 0 heap allocations");
}

static void test_zero_alloc_with_body_response() {
    // Responses with a "body" field still allocate 1 unique_ptr for the
    // sub-reader from get_object("body"). This test documents that behavior.
    note::backends::BufferJsonBackend<1024, 128> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v1","body":{"org":"blues","product":"app"}})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    { api.card.version().execute(); }

    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(r.has_value());
    assert(r.body() != nullptr);
    assert(r.body()->get_string("org") == "blues");

    stats.print("card.version with body");
    // body sub-reader costs 1 allocation (get_object returns unique_ptr)
    std::printf("    body response allocs: %zu (expected: 1 for sub-reader)\n", stats.allocs);
    assert(stats.allocs == 1);
    std::puts("  PASS: zero_alloc_with_body — 1 alloc for body sub-reader only");
}

static void test_zero_alloc_no_leaks() {
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"version":"v1"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    { api.card.version().execute(); }

    TrackingScope scope;
    {
        auto r = api.card.version().execute();
        assert(r.has_value());
        // r goes out of scope here
    }
    auto stats = scope.finish();

    stats.print("leak detection");
    assert(stats.allocs == 0);
    assert(stats.leaked_bytes == 0);
    std::puts("  PASS: zero_alloc_no_leaks — 0 allocations, 0 bytes leaked");
}

static void test_zero_alloc_error_response() {
    // Error responses use parse_response() (allocating) to keep error string alive.
    note::backends::BufferJsonBackend<512, 64> backend;
    ScriptedTransport transport;
    transport.response = R"({"err":"file not found"})";

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Warm up
    transport.response = R"({})";
    { api.card.version().execute(); }

    // Error path: allocates for owned reader
    transport.response = R"({"err":"file not found"})";
    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(!r.has_value());
    assert(r.error().code == note::Error::Notecard);
    assert(r.error().message == "file not found");

    stats.print("error response");
    std::printf("    error path allocs: %zu (expected: >0 for error string lifetime)\n",
                stats.allocs);
    // Error path requires allocations to keep error message alive — that's by design
    assert(stats.allocs > 0);
    std::puts("  PASS: error_response — allocations expected on error path");
}

// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::puts("=== Zero-allocation profiling tests (buffer backend) ===\n");

    test_zero_alloc_card_version();
    test_zero_alloc_hub_set();
    test_zero_alloc_multiple_requests();
    test_zero_alloc_with_body_response();
    test_zero_alloc_no_leaks();
    test_zero_alloc_error_response();

    std::puts("\nAll zero-allocation profiling tests passed.");
    std::puts("BufferJsonBackend + string_view transport = ZERO heap allocations in steady state.");
    return 0;
}
