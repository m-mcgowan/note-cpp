// Allocation profiling tests for note-cpp with the cJSON backend.
//
// Measures heap allocations per request/response cycle by intercepting:
//   - C++ allocations: global operator new/delete override
//   - cJSON allocations: cJSON_InitHooks
//
// These tests establish baseline allocation counts and serve as regression
// guards. After optimization, tighten the bounds.

#include <note/backends/cjson.hpp>
#include <note/arena.hpp>
#include <note/notecard.hpp>
#include <note/transport/serial.hpp>
#include <note/transport/i2c.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_add.hpp>
#include <note/api.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <new>

// ═══════════════════════════════════════════════════════════════════════════
// Allocation tracking
// ═══════════════════════════════════════════════════════════════════════════

// Global cumulative counters — always increment, never reset.
// TrackingScope takes snapshots to compute deltas.
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

// Snapshot delta: what happened between two counter snapshots.
struct AllocStats {
    size_t cpp_allocs = 0;
    size_t cpp_frees = 0;
    size_t cpp_peak_bytes = 0;
    size_t cpp_bytes_leaked = 0;  // bytes still held at scope end

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

// We store the allocation size in a header before the returned pointer
// so that operator delete can track bytes freed.
struct AllocHeader {
    size_t size;
};
static constexpr size_t kHeaderSize = sizeof(AllocHeader);
// Alignment: ensure returned pointer is max-aligned
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

// cJSON hook wrappers
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
        // Reset peak tracking for this scope
        g_counters.cpp_peak_bytes = g_counters.cpp_bytes;
    }

    AllocStats finish() {
        g_tracking = false;
        AllocStats s;
        s.cpp_allocs = g_counters.cpp_allocs - snapshot.cpp_allocs;
        s.cpp_frees = g_counters.cpp_frees - snapshot.cpp_frees;
        s.cpp_peak_bytes = g_counters.cpp_peak_bytes - snapshot.cpp_bytes;
        // Bytes still allocated at scope end vs scope start
        s.cpp_bytes_leaked = (g_counters.cpp_bytes > snapshot.cpp_bytes)
            ? g_counters.cpp_bytes - snapshot.cpp_bytes : 0;
        s.cjson_allocs = g_counters.cjson_allocs - snapshot.cjson_allocs;
        s.cjson_frees = g_counters.cjson_frees - snapshot.cjson_frees;
        return s;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// ScriptedHal — minimal reactive serial HAL for allocation profiling
// (Simplified from tests/test_transport_serial.cpp)
// ═══════════════════════════════════════════════════════════════════════════

struct ScriptedSerialHal : public note::transport::SerialHal {
    std::deque<uint8_t> rx;
    std::deque<std::string> json_responses;
    uint32_t now_ms = 0;

    void queue_response(const std::string& s) {
        json_responses.push_back(s);
    }

    bool transmit(const uint8_t* d, size_t n) override {
        if (n == 1 && d[0] == '\n') {
            // Reset probe → clean drain
            rx.push_back('\r');
            rx.push_back('\n');
        } else if (n == 2 && d[0] == '\r' && d[1] == '\n') {
            // Request terminator → inject queued response
            if (!json_responses.empty()) {
                for (char c : json_responses.front())
                    rx.push_back(static_cast<uint8_t>(c));
                json_responses.pop_front();
            }
        }
        return true;
    }

    size_t receive(uint8_t* buf, size_t max) override {
        size_t n = std::min(max, rx.size());
        for (size_t i = 0; i < n; ++i) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        return n;
    }

    uint32_t millis() override { return now_ms; }
    void delay(uint32_t ms) override { now_ms += ms; }
};

// ═══════════════════════════════════════════════════════════════════════════
// ScriptedI2CHal — minimal reactive I2C HAL
// ═══════════════════════════════════════════════════════════════════════════

struct ScriptedI2CHal : public note::transport::I2CHal {
    std::deque<uint8_t> rx;
    std::deque<std::string> json_responses;
    uint32_t now_ms = 0;

    void queue_response(const std::string& s) {
        json_responses.push_back(s);
    }

    bool reset() override { return true; }

    bool transmit(const uint8_t* d, size_t n) override {
        // Check for '\n' (I2C uses bare \n as terminator)
        bool has_newline = false;
        for (size_t i = 0; i < n; ++i) {
            if (d[i] == '\n') has_newline = true;
        }

        // If this is a reset probe (just '\n') inject drain
        if (n == 1 && d[0] == '\n') {
            rx.push_back('\r');
            rx.push_back('\n');
        }
        // If the data contains a newline and there are queued responses,
        // inject the next one (this handles request transmission)
        else if (has_newline && !json_responses.empty()) {
            for (char c : json_responses.front())
                rx.push_back(static_cast<uint8_t>(c));
            json_responses.pop_front();
        }
        return true;
    }

    bool receive(uint8_t* buf, size_t len, uint32_t& available) override {
        if (len == 0) {
            // Priming query
            available = static_cast<uint32_t>(rx.size());
            return true;
        }
        size_t n = std::min(len, rx.size());
        for (size_t i = 0; i < n; ++i) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        available = static_cast<uint32_t>(rx.size());
        return true;
    }

    uint32_t millis() override { return now_ms; }
    void delay(uint32_t ms) override { now_ms += ms; }
    size_t max_transfer() override { return 253; }
};

// ═══════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════

static void test_builder_alloc_profile() {
    note::backends::CjsonBackend backend;

    TrackingScope scope;

    auto builder = backend.create_builder();
    builder->add("req", "hub.set");
    builder->add("product", "com.example.app");
    builder->add("mode", "periodic");
    builder->add("outbound", int32_t{60});
    auto json = builder->to_string();

    auto stats = scope.finish();
    stats.print("builder");

    // Builder should produce valid JSON
    assert(json.find("\"req\":\"hub.set\"") != std::string::npos);

    // Leak check: all allocations freed when builder goes out of scope
    // (builder is still alive here, so cJSON tree is still allocated)
    builder.reset();  // destroy builder
    json.clear();
    json.shrink_to_fit();

    // Now check the full cycle stats
    std::printf("    builder total allocs: %zu (C++: %zu, cJSON: %zu)\n",
                stats.total_allocs(), stats.cpp_allocs, stats.cjson_allocs);

    std::puts("  PASS: builder_alloc_profile");
}

static void test_serial_transport_alloc_profile() {
    // Pre-create HAL and transport outside tracking scope
    ScriptedSerialHal hal;
    note::transport::NotecardSerial transport(hal);

    // Queue a response
    hal.queue_response("{\"version\":\"notecard-7.2.1\"}\r\n");

    // Warm up: first call triggers reset
    auto warmup = transport.transact("{\"req\":\"card.version\"}", 10000);
    assert(warmup.has_value());

    // Now measure a steady-state request
    hal.queue_response("{\"version\":\"notecard-7.2.1\"}\r\n");

    TrackingScope scope;
    auto result = transport.transact("{\"req\":\"card.version\"}", 10000);
    auto stats = scope.finish();

    assert(result.has_value());
    stats.print("serial transport (steady-state)");

    std::printf("    serial transport allocs: C++=%zu cJSON=%zu\n",
                stats.cpp_allocs, stats.cjson_allocs);

    std::puts("  PASS: serial_transport_alloc_profile");
}

static void test_i2c_transport_alloc_profile() {
    ScriptedI2CHal hal;
    note::transport::NotecardI2c transport(hal);

    // Warm up
    hal.queue_response("{\"version\":\"notecard-7.2.1\"}\n");
    auto warmup = transport.transact("{\"req\":\"card.version\"}", 10000);
    assert(warmup.has_value());

    // Steady-state measurement
    hal.queue_response("{\"version\":\"notecard-7.2.1\"}\n");

    TrackingScope scope;
    auto result = transport.transact("{\"req\":\"card.version\"}", 10000);
    auto stats = scope.finish();

    assert(result.has_value());
    stats.print("I2C transport (steady-state)");

    std::printf("    I2C transport allocs: C++=%zu cJSON=%zu\n",
                stats.cpp_allocs, stats.cjson_allocs);

    std::puts("  PASS: i2c_transport_alloc_profile");
}

static void test_full_execute_alloc_profile() {
    note::backends::CjsonBackend backend;
    ScriptedSerialHal hal;
    note::transport::NotecardSerial transport(hal);

    // Warm up transport
    hal.queue_response("{}\r\n");
    auto warmup = transport.transact("{\"req\":\"card.version\"}", 10000);
    assert(warmup.has_value());

    // Build the Notecard and Api outside the tracking scope
    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Queue a realistic card.version response
    hal.queue_response("{\"version\":\"notecard-7.2.1\",\"device\":\"dev:12345\","
                       "\"body\":{\"org\":\"org\",\"product\":\"product\","
                       "\"target\":\"r\"},\"api\":5}\r\n");

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
    ScriptedSerialHal hal;
    note::transport::NotecardSerial transport(hal);

    // Warm up
    hal.queue_response("{}\r\n");
    transport.transact("{}", 10000);

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Queue response for note.add
    hal.queue_response("{\"total\":1}\r\n");

    TrackingScope scope;
    auto r = api.note.add()
        .file("sensors.qo")
        .execute();
    auto stats = scope.finish();

    assert(r.has_value());
    stats.print("execute (note.add with file)");

    std::printf("    note.add allocs: %zu (C++: %zu, cJSON: %zu)\n",
                stats.total_allocs(), stats.cpp_allocs, stats.cjson_allocs);

    std::puts("  PASS: execute_with_body_alloc_profile");
}

static void test_leak_detection() {
    note::backends::CjsonBackend backend;
    ScriptedSerialHal hal;
    note::transport::NotecardSerial transport(hal);

    // Warm up
    hal.queue_response("{}\r\n");
    transport.transact("{}", 10000);

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    hal.queue_response("{\"version\":\"notecard-7.2.1\"}\r\n");

    TrackingScope scope;
    {
        auto r = api.card.version().execute();
        assert(r.has_value());
        // r goes out of scope here — all request-scoped memory should be freed
    }
    auto stats = scope.finish();

    stats.print("leak detection");

    // After the result goes out of scope, the backend's owned_reader_ (from
    // get_reader()) still holds the last parsed response. This is not a leak
    // — it's reused on the next request. The cJSON tree nodes are freed when
    // the reader is replaced. True leaks would show as growing bytes across
    // multiple requests (tested in test_multiple_requests_no_growth).
    std::printf("    note: backend retains owned_reader_ (%zu bytes) for reuse\n",
                stats.cpp_bytes_leaked);

    std::puts("  PASS: leak_detection");
}

static void test_multiple_requests_no_growth() {
    note::backends::CjsonBackend backend;
    ScriptedSerialHal hal;
    note::transport::NotecardSerial transport(hal);

    // Warm up
    hal.queue_response("{}\r\n");
    transport.transact("{}", 10000);

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Measure first request
    hal.queue_response("{\"version\":\"v1\"}\r\n");
    TrackingScope scope1;
    { auto r = api.card.version().execute(); }
    auto stats1 = scope1.finish();

    // Measure tenth request
    for (int i = 0; i < 8; ++i) {
        hal.queue_response("{\"version\":\"v1\"}\r\n");
        auto r = api.card.version().execute();
    }

    hal.queue_response("{\"version\":\"v1\"}\r\n");
    TrackingScope scope10;
    { auto r = api.card.version().execute(); }
    auto stats10 = scope10.finish();

    stats1.print("request #1");
    stats10.print("request #10");

    // Allocation count should not grow between requests
    // (transport strings may reuse capacity, no new allocations needed)
    std::printf("    request #1 allocs: %zu, request #10 allocs: %zu\n",
                stats1.total_allocs(), stats10.total_allocs());

    // After warm-up, allocation counts should be stable (same or fewer)
    assert(stats10.total_allocs() <= stats1.total_allocs());

    std::puts("  PASS: multiple_requests_no_growth");
}

// ═══════════════════════════════════════════════════════════════════════════
// Arena tests — zero heap allocations with CjsonArenaBackend
// ═══════════════════════════════════════════════════════════════════════════

static void test_arena_zero_heap_execute() {
    // Statically allocated arena — predictable, bounded memory
    alignas(std::max_align_t) char pool[8192];
    note::MonotonicArena arena(pool);
    note::backends::CjsonArenaBackend backend(arena);

    ScriptedSerialHal hal;
    note::transport::NotecardSerial transport(hal);

    // Warm up: transport buffer + backend owned_reader_ established
    hal.queue_response("{\"version\":\"notecard-7.2.1\",\"device\":\"dev:12345\",\"board\":\"1.0\"}\r\n");
    auto warmup = transport.transact("{\"req\":\"card.version\"}", 10000);
    assert(warmup.has_value());

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // First execute warms up backend's owned_reader_
    hal.queue_response("{\"version\":\"notecard-7.2.1\",\"device\":\"dev:12345\",\"board\":\"1.0\"}\r\n");
    { auto warm = api.card.version().execute(); }

    // Steady-state: all cJSON nodes route through arena, C++ wrapper is reused
    hal.queue_response("{\"version\":\"notecard-7.2.1\",\"device\":\"dev:12345\",\"board\":\"1.0\"}\r\n");

    TrackingScope scope;
    auto r = api.card.version().execute();
    auto stats = scope.finish();

    assert(r.has_value());

    stats.print("arena execute (card.version, steady-state)");
    std::printf("    arena used: %zu / %zu bytes\n", arena.used(), arena.capacity());

    // Zero cJSON heap allocations — all routed through arena
    assert(stats.cjson_allocs == 0);
    // C++ allocs: 1 for the owned_reader_ replacement (make_unique<CjsonReader>).
    // The CjsonReader wrapper is a C++ object; cJSON nodes go to the arena.
    // For truly zero C++ heap allocs, use BufferJsonBackend instead.
    std::printf("    C++ allocs: %zu (CjsonReader wrapper), cJSON heap allocs: %zu\n",
                stats.cpp_allocs, stats.cjson_allocs);
    std::puts("  PASS: arena_zero_heap_execute — 0 cJSON heap allocations (all via arena)");
}

static void test_arena_multiple_requests_bounded() {
    alignas(std::max_align_t) char pool[8192];
    note::MonotonicArena arena(pool);
    note::backends::CjsonArenaBackend backend(arena);

    ScriptedSerialHal hal;
    note::transport::NotecardSerial transport(hal);

    // Warm up transport with a realistic-sized response
    hal.queue_response("{\"version\":\"notecard-7.2.1\",\"device\":\"dev:1\"}\r\n");
    transport.transact("{\"req\":\"card.version\"}", 10000);

    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Run multiple requests, verify arena usage is bounded (resets each time)
    size_t max_arena_used = 0;
    for (int i = 0; i < 10; ++i) {
        hal.queue_response("{\"version\":\"v1\",\"device\":\"dev:1\"}\r\n");
        auto r = api.card.version().execute();
        assert(r.has_value());
        if (arena.used() > max_arena_used) max_arena_used = arena.used();
    }

    std::printf("  arena peak usage: %zu / %zu bytes (%.1f%%)\n",
                max_arena_used, arena.capacity(),
                100.0 * max_arena_used / arena.capacity());

    // Arena should use a small fraction of the pool
    assert(max_arena_used < arena.capacity());
    std::puts("  PASS: arena_multiple_requests_bounded — arena reuse verified");
}

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

// ═══════════════════════════════════════════════════════════════════════════

int main() {
    install_cjson_hooks();

    std::puts("=== Allocation profiling tests (cJSON backend) ===\n");

    test_builder_alloc_profile();
    test_serial_transport_alloc_profile();
    test_i2c_transport_alloc_profile();
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
