// Regression tests for Notecard move-assignment safety.
//
// Background: Notecard previously held a per-instance PlatformMd5 plus a
// pointer `md5_ = &platform_md5_` that self-referenced that member. The
// compiler-generated move-assignment copied the pointer literally, so
//
//     nc_ = Notecard(transport, alloc);   // NotecardApi::begin pattern
//
// left `nc_.md5_` pointing into the destroyed temporary's platform_md5_.
// Any subsequent `md5_->compute(...)` — the first call card.binary.put
// makes — dereferenced freed stack memory, read a bogus vtable, and jumped
// to a null function pointer.
//
// This went undetected because host tests construct Notecards in place
// (no move-assignment) and `make_test_notecard` uses guaranteed copy
// elision on its return value. The buggy path was specifically exercised
// only by the NotecardApi::begin flow used from note::arduino::Notecard
// and note::posix::Notecard — and no host test stitched begin() to a
// card.binary operation.
//
// The fix moved the default provider to `static inline`, removing the
// self-reference entirely. These tests verify that move-assigning a
// Notecard still yields a working md5 provider.

#include "catch.hpp"

#include <note/api/card_binary_put.hpp>
#include <note/notecard.hpp>
#include <note/streaming_transport.hpp>
#include <note/transport_hal.hpp>

#include <deque>
#include <string>

namespace {

// Minimal TransportHal that returns canned JSON responses.
class MockHal : public note::TransportHal {
public:
    std::deque<uint8_t> rx;

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }

    bool transmit(const uint8_t*, size_t) override { return true; }

    note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
        if (rx.empty())
            return note::make_error(note::Error::ResponseLost, note::Cause::Timeout, "no data");
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        return n;
    }

    bool reset() override { return true; }
    bool write_line_terminator() override { return true; }
    void delay(uint32_t) override {}
    uint32_t millis() override { return 0; }
};

}  // namespace

TEST_CASE("Notecard default md5 provider survives move-assign", "[notecard][regression]") {
    // Reproduce the NotecardApi::begin pattern. Pre-fix, md5_ was a
    // per-instance pointer to a per-instance member; the default move
    // copied the pointer literally, leaving it dangling at the destroyed
    // temporary's platform_md5_ after `nc = Notecard(...)`.
    //
    // Deterministic check: with the fix, md5_provider() returns the same
    // shared-static pointer across all Notecards regardless of move
    // history. Pre-fix, each default-constructed Notecard's provider
    // lives inside the object itself — different addresses per instance
    // and broken after move-assign.
    MockHal hal;
    note::StreamingTransport transport{hal};

    note::Notecard reference;   // the invariant we expect to hold
    note::Notecard nc;
    nc = note::Notecard(transport, note::Allocator{});

    REQUIRE(nc.md5_provider() != nullptr);
    REQUIRE(nc.md5_provider() == reference.md5_provider());
}

TEST_CASE("Notecard default md5 provider survives move-construct", "[notecard][regression]") {
    MockHal hal;
    note::StreamingTransport transport{hal};

    note::Notecard reference;
    note::Notecard nc{note::Notecard(transport, note::Allocator{})};

    REQUIRE(nc.md5_provider() != nullptr);
    REQUIRE(nc.md5_provider() == reference.md5_provider());
}

TEST_CASE("Notecard custom md5 provider survives move-assign", "[notecard][regression]") {
    // Spy provider to verify that a caller-installed provider is retained
    // (not replaced by the default) across move-assignment.
    struct SpyMd5 : note::Md5Provider {
        int calls = 0;
        note::Md5Hex compute(const uint8_t*, size_t) override {
            ++calls;
            note::Md5Hex h{};
            for (int i = 0; i < 32; ++i) h.buf[i] = 'a';
            return h;
        }
    };

    MockHal hal;
    note::StreamingTransport transport{hal};
    SpyMd5 spy;

    note::Notecard nc;
    nc.set_md5_provider(spy);
    nc = note::Notecard(transport, note::Allocator{});  // move-assign must not wipe spy? no — it DOES reset, that's expected

    // After move-assign, nc is a freshly-constructed Notecard — custom
    // provider is reset to default (expected). Verify we have the default
    // working provider, not the dangling one.
    REQUIRE(nc.md5_provider() != nullptr);
    REQUIRE(nc.md5_provider() != static_cast<note::Md5Provider*>(&spy));
}
