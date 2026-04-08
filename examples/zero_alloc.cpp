// Zero-allocation patterns for note-cpp.
//
// Demonstrates three approaches to eliminating heap allocation in request/response
// cycles, suitable for hard real-time or memory-constrained embedded systems.
//
// Pattern 1: BufferJsonBackend — truly zero heap allocation.
//   All JSON building and parsing uses fixed member buffers. Transport returns
//   string_view into its member buffer. Response string_views point into the
//   transport buffer (valid until next execute() call).
//
// Pattern 2: StringPool — arena-backed response strings that survive reuse.
//   set_allocator() copies response string_views into a MonotonicArena so they
//   remain valid after the next execute() call. No heap allocation.
//
// Pattern 3: CjsonArenaBackend — predictable, bounded memory via arena.
//   cJSON allocations route through a MonotonicArena with a statically allocated
//   buffer. No heap fragmentation; memory usage is bounded and visible.
//
// Build: c++ -std=c++20 -I include examples/zero_alloc.cpp

#include <note/backends/buffer.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/notecard.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api.hpp>

#include <cstdio>
#include <functional>
#include <string>

// ── Mock transport (returns string_view into member buffer) ─────────────────

struct MockTransport : note::ITransport {
    std::string response_buf;

    note::Result<note::string_view> transact(note::string_view request, uint32_t) override {
        // In a real system, this would send `request` over serial/I2C and
        // receive the response into response_buf. The string_view is valid
        // until the next call.
        (void)request;
        return note::string_view(response_buf);
    }
    note::Result<void> send(note::string_view) override { return {}; }
    void reset() override {}
    void abort() override {}
    uint32_t millis() override { return 0; }
    void delay(uint32_t) override {}
};

// ── Pattern 1: BufferJsonBackend — zero heap allocation ─────────────────────

static void demo_buffer_backend() {
    std::puts("=== Pattern 1: BufferJsonBackend (zero heap allocation) ===\n");

    // Template parameters: build buffer size, max jsmn tokens
    note::backends::BufferJsonBackend<512, 64> backend;

    // Transport with member buffer (string_view return)
    MockTransport transport;
    transport.response_buf = R"({"version":"notecard-7.2.1","device":"dev:12345","board":"1.0"})";

    // Notecard and Api — all state is on the stack or in members
    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Execute a request — ZERO heap allocations in steady state.
    // The request is built into backend's member buffer.
    // The transport returns string_view into its member buffer.
    // The response is parsed using backend's member jsmn tokens.
    // Response string_views point into the transport buffer.
    auto r = api.card.version().execute();
    if (r.has_value()) {
        std::printf("  version: %.*s\n", (int)r.version.size(), r.version.data());
        std::printf("  device:  %.*s\n", (int)r.device.size(), r.device.data());
        std::printf("  board:   %.*s\n", (int)r.board.size(), r.board.data());
    }

    // IMPORTANT: Response string_views are valid until the next execute() call.
    // If you need to persist values, copy them to std::string before calling
    // execute() again:
    //   std::string saved_version(r.version);

    // Multiple requests reuse the same buffers — no growth, no fragmentation.
    transport.response_buf = R"({})";
    auto r2 = api.hub.set().mode("periodic").outbound(int32_t{60}).execute();
    if (r2.has_value()) {
        std::puts("  hub.set: OK");
    }
    // Note: r.version is now invalid (transport buffer was reused)

    std::puts("");
}

// ── Pattern 2: StringPool — arena-backed response strings ────────────────────

static void demo_string_pool() {
    std::puts("=== Pattern 2: StringPool (arena-backed response strings) ===\n");

    note::backends::BufferJsonBackend<512, 64> backend;

    MockTransport transport;
    note::Notecard nc(backend, transport);

    // Configure a MonotonicArena for string interning.
    // When set, execute() copies response string_views into the arena
    // so they survive transport buffer reuse.
    char arena_buf[1024];
    note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
    nc.set_allocator(note::arena_allocator(arena));

    note::Api api(nc);

    // First request
    transport.response_buf = R"({"version":"notecard-7.2.1","device":"dev:12345"})";
    auto r1 = api.card.version().execute();

    // Second request — transport buffer is overwritten
    transport.response_buf = R"({"version":"notecard-8.0.0","device":"dev:99999"})";
    auto r2 = api.card.version().execute();

    // r1's strings are still valid — they live in the arena, not the transport buffer
    if (r1.has_value()) {
        std::printf("  r1 version: %.*s\n", (int)r1.version.size(), r1.version.data());
        std::printf("  r1 device:  %.*s\n", (int)r1.device.size(), r1.device.data());
    }
    if (r2.has_value()) {
        std::printf("  r2 version: %.*s\n", (int)r2.version.size(), r2.version.data());
        std::printf("  r2 device:  %.*s\n", (int)r2.device.size(), r2.device.data());
    }

    std::printf("  arena: %zu / %zu bytes used\n", arena.used(), arena.capacity());

    // Reset the arena when you're done with previous responses.
    // After reset, r1 and r2 string_views are invalid.
    arena.reset();
    nc.set_allocator(note::arena_allocator(arena));

    std::puts("");
}

// ── Pattern 3: CjsonArenaBackend — bounded arena memory ────────────────────
//
// This pattern requires cJSON, which is not available in this header-only
// example. The code below shows the pattern — uncomment when cJSON is linked.
//
//   #include <note/backends/cjson.hpp>
//   #include <note/arena.hpp>
//
//   static void demo_arena_backend() {
//       // Statically allocated arena — no heap, bounded memory
//       alignas(std::max_align_t) char pool[4096];
//       note::MonotonicArena arena(pool);
//
//       // cJSON routes all allocations through the arena
//       note::backends::CjsonArenaBackend backend(arena);
//
//       MockTransport transport;
//       transport.response_buf = R"({"version":"notecard-7.2.1"})";
//
//       note::Notecard nc(backend, transport);
//       note::Api api(nc);
//
//       auto r = api.card.version().execute();
//       if (r.has_value()) {
//           printf("  version: %.*s\n", (int)r.version.size(), r.version.data());
//       }
//
//       // Arena stats: see exactly how much memory was used
//       printf("  arena: %zu / %zu bytes used (%.1f%%)\n",
//              arena.used(), arena.capacity(),
//              100.0 * arena.used() / arena.capacity());
//
//       // Arena resets between requests — memory is bounded.
//       // Each execute() call reuses the same pool.
//   }

int main() {
    demo_buffer_backend();
    demo_string_pool();

    std::puts("=== Pattern 3: CjsonArenaBackend ===\n");
    std::puts("  (Requires cJSON — see integration tests for live demo)\n");
    std::puts("  See: tests/integration/cjson/test_alloc_profile.cpp");

    std::puts("\nKey constraints for zero-allocation operation:");
    std::puts("  1. Use BufferJsonBackend or CjsonArenaBackend (not CjsonBackend)");
    std::puts("  2. Transport must return string_view into member buffer");
    std::puts("  3. Use set_allocator() if response strings must survive buffer reuse");
    std::puts("  4. Reset the arena between request batches to bound memory");

    return 0;
}
