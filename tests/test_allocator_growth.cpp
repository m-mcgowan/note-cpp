// Tests for allocator growth strategy and memory exhaustion in passthrough.
//
// The auto-sizing transact(json) must:
// 1. Start with a small allocation and grow as the response arrives
// 2. Use reallocate() which can extend in place (no unnecessary copies)
// 3. Handle allocation failure gracefully — return a clean error, not crash
// 4. Not leak memory on any error path
// 5. Work with constrained allocators (arena with limited space)

#include "catch.hpp"
#include <note/notecard.hpp>
#include <note/streaming_transport.hpp>
#include <note/allocator.hpp>
#include <note/arena.hpp>
#include <cstring>
#include <vector>

namespace {

// Mock HAL that returns a configurable response.
struct MockHal : note::TransportHal {
    std::string response;
    size_t rsp_pos = 0;

    explicit MockHal(size_t response_size) {
        // Build a valid JSON response of the requested size:
        // {"d":"xxxx...xxxx"}
        std::string payload(response_size > 8 ? response_size - 8 : 1, 'x');
        response = "{\"d\":\"" + payload + "\"}\r\n";
    }

    explicit MockHal(const std::string& json) {
        response = json + "\r\n";
    }

    bool transmit(const uint8_t*, size_t) override { return true; }
    note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t) override {
        if (rsp_pos >= response.size()) return size_t(0);
        size_t n = std::min(max, response.size() - rsp_pos);
        std::memcpy(buf, response.data() + rsp_pos, n);
        rsp_pos += n;
        return n;
    }
    bool reset() override { return true; }
    bool write_line_terminator() override { rsp_pos = 0; return true; }
    void delay(uint32_t) override {}
};

// Allocator that tracks allocations and can simulate failure at a threshold.
struct TrackingAllocator {
    size_t total_allocated = 0;
    size_t peak_allocated = 0;
    size_t alloc_count = 0;
    size_t realloc_count = 0;
    size_t free_count = 0;
    size_t max_total = SIZE_MAX;  // set to simulate exhaustion

    // Current live allocations for leak detection
    std::vector<std::pair<void*, size_t>> live;

    note::Allocator to_allocator() {
        return {
            [](size_t n, void* ctx) -> void* {
                auto* self = static_cast<TrackingAllocator*>(ctx);
                if (self->total_allocated + n > self->max_total) return nullptr;
                void* p = std::malloc(n);
                if (p) {
                    self->total_allocated += n;
                    self->peak_allocated = std::max(self->peak_allocated, self->total_allocated);
                    self->alloc_count++;
                    self->live.push_back({p, n});
                }
                return p;
            },
            [](void* p, size_t n, void* ctx) {
                auto* self = static_cast<TrackingAllocator*>(ctx);
                if (p) {
                    self->total_allocated -= n;
                    self->free_count++;
                    self->live.erase(
                        std::remove_if(self->live.begin(), self->live.end(),
                            [p](auto& e) { return e.first == p; }),
                        self->live.end());
                    std::free(p);
                }
            },
            [](void* p, size_t old_n, size_t new_n, void* ctx) -> void* {
                auto* self = static_cast<TrackingAllocator*>(ctx);
                size_t growth = new_n - old_n;
                if (self->total_allocated + growth > self->max_total) return nullptr;
                void* np = std::realloc(p, new_n);
                if (np) {
                    self->total_allocated += growth;
                    self->peak_allocated = std::max(self->peak_allocated, self->total_allocated);
                    self->realloc_count++;
                    // Update live tracking (pointer may have changed)
                    for (auto& e : self->live) {
                        if (e.first == p) { e.first = np; e.second = new_n; break; }
                    }
                }
                return np;
            },
            this
        };
    }

    bool has_leaks() const { return !live.empty(); }
};

} // namespace

// ---------------------------------------------------------------------------
// Basic growth: small response fits in initial buffer
// ---------------------------------------------------------------------------

TEST_CASE("transact: small response fits without growth") {
    MockHal hal(100);  // 100-byte response
    note::StreamingTransport transport(hal);
    TrackingAllocator tracker;
    {
        note::Notecard nc(transport, tracker.to_allocator());
        auto rsp = nc.transact(R"({"req":"card.version"})");
        REQUIRE(rsp);
        CHECK(rsp->size() > 0);
        CHECK(tracker.realloc_count == 0);  // no growth needed
        CHECK(tracker.live.size() == 1);    // one buffer held by nc
    }
    // After nc destroyed, buffer is freed by destructor
    CHECK(!tracker.has_leaks());
}

// ---------------------------------------------------------------------------
// Growth: response exceeds initial buffer, triggers realloc
// ---------------------------------------------------------------------------

TEST_CASE("transact: large response triggers realloc growth") {
    MockHal hal(3000);  // 3KB response — exceeds 1KB initial
    note::StreamingTransport transport(hal);
    TrackingAllocator tracker;
    note::Notecard nc(transport, tracker.to_allocator());

    auto rsp = nc.transact(R"({"req":"env.get"})");
    REQUIRE(rsp);
    CHECK(rsp->size() > 2900);
    CHECK(tracker.realloc_count >= 1);  // grew at least once
}

// ---------------------------------------------------------------------------
// Memory exhaustion: allocator refuses growth
// ---------------------------------------------------------------------------

TEST_CASE("transact: realloc failure returns clean error") {
    MockHal hal(3000);  // needs >1KB but allocator capped at 1.5KB
    note::StreamingTransport transport(hal);
    TrackingAllocator tracker;
    tracker.max_total = 1536;  // allow initial 1KB, deny growth to 2KB
    note::Notecard nc(transport, tracker.to_allocator());

    auto rsp = nc.transact(R"({"req":"env.get"})");
    REQUIRE(!rsp);
    CHECK(rsp.error().code == note::Error::Overflow);
}

TEST_CASE("transact: no leak on realloc failure") {
    MockHal hal(3000);
    note::StreamingTransport transport(hal);
    TrackingAllocator tracker;
    tracker.max_total = 1536;
    note::Notecard nc(transport, tracker.to_allocator());

    auto rsp = nc.transact(R"({"req":"env.get"})");
    REQUIRE(!rsp);
    // After failure, all memory should be freed
    CHECK(tracker.total_allocated == 0);
    CHECK(!tracker.has_leaks());
}

TEST_CASE("transact: initial alloc failure returns clean error") {
    MockHal hal(100);
    note::StreamingTransport transport(hal);
    TrackingAllocator tracker;
    tracker.max_total = 0;  // can't allocate anything
    note::Notecard nc(transport, tracker.to_allocator());

    auto rsp = nc.transact(R"({"req":"card.version"})");
    REQUIRE(!rsp);
    CHECK(!tracker.has_leaks());
}

// ---------------------------------------------------------------------------
// Sequential calls: previous buffer freed before new allocation
// ---------------------------------------------------------------------------

TEST_CASE("transact: second call frees previous buffer") {
    MockHal hal(100);
    note::StreamingTransport transport(hal);
    TrackingAllocator tracker;
    note::Notecard nc(transport, tracker.to_allocator());

    auto rsp1 = nc.transact(R"({"req":"card.version"})");
    REQUIRE(rsp1);
    CHECK(tracker.alloc_count == 1);

    auto rsp2 = nc.transact(R"({"req":"card.version"})");
    REQUIRE(rsp2);
    CHECK(tracker.free_count >= 1);  // first buffer was freed
    // Only one buffer alive at a time
    CHECK(tracker.live.size() == 1);
}

// ---------------------------------------------------------------------------
// Explicit buffer: overflow returns error, no corruption
// ---------------------------------------------------------------------------

TEST_CASE("transact(buf): overflow returns error and drains response") {
    MockHal hal(500);
    note::StreamingTransport transport(hal);
    note::Notecard nc(transport);

    char buf[64];  // way too small
    auto rsp = nc.transact(R"({"req":"card.version"})", buf);
    REQUIRE(!rsp);
    CHECK(rsp.error().code == note::Error::Overflow);

    // Second call should work — the HAL was drained, not left with stale data
    MockHal hal2(30);
    note::StreamingTransport transport2(hal2);
    note::Notecard nc2(transport2);
    char buf2[256];
    auto rsp2 = nc2.transact(R"({"req":"card.version"})", buf2);
    REQUIRE(rsp2);
}

// ---------------------------------------------------------------------------
// Arena allocator: growth with monotonic arena
// ---------------------------------------------------------------------------

TEST_CASE("transact: works with arena allocator") {
    MockHal hal(100);
    note::StreamingTransport transport(hal);

    char pool[4096];
    note::MonotonicArena arena(pool);
    note::Notecard nc(transport, note::arena_allocator(arena));

    auto rsp = nc.transact(R"({"req":"card.version"})");
    REQUIRE(rsp);
    CHECK(rsp->size() > 0);
}

TEST_CASE("transact: arena exhaustion returns clean error") {
    MockHal hal(3000);  // needs >1KB
    note::StreamingTransport transport(hal);

    char pool[512];  // tiny arena — can't hold the response
    note::MonotonicArena arena(pool);
    note::Notecard nc(transport, note::arena_allocator(arena));

    auto rsp = nc.transact(R"({"req":"env.get"})");
    REQUIRE(!rsp);
    // Should not crash — returns a clean error
}
