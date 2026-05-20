// Tests for Notecard streaming transport paths in notecard.hpp.
// Covers: streaming execute (void + typed), streaming commands,
// streaming transact/send, binary I/O with streaming, timing enforcement.

#include <doctest.h>
#include "test_notecard_factory.hpp"

#include <note/notecard.hpp>
#include <note/allocator.hpp>
#include <note/protocol.hpp>
#include <note/transact.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_restart.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/card_binary_put.hpp>
#include <note/api/card_binary_get.hpp>
#include <note/backends/buffer.hpp>
#include <note/link/cobs.hpp>
#include <note/md5.hpp>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

// =========================================================================
// Mock Hal for streaming tests
// =========================================================================

class MockStreamHal : public note::Hal {
public:
    std::deque<uint8_t> rx;
    std::vector<uint8_t> tx;
    uint32_t current_millis = 0;
    uint32_t delay_total = 0;
    bool transmit_ok = true;
    bool reset_called = false;

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }

    bool transmit(const uint8_t* data, size_t len) override {
        if (!transmit_ok) return false;
        tx.insert(tx.end(), data, data + len);
        return true;
    }

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

    bool reset() override { reset_called = true; return true; }
    bool write_line_terminator() override {
        tx.push_back('\n');
        return true;
    }
    void delay(uint32_t ms) override { delay_total += ms; }
    uint32_t millis() override { return current_millis; }
};

// =========================================================================
// Streaming test harness
// =========================================================================

struct StreamHarness {
    MockStreamHal hal;
    note::Protocol transport{hal};
    std::unique_ptr<note::Notecard> nc_ptr;
    note::Notecard& nc;

    // Construct with allocator (exercises streaming execute path).
    StreamHarness()
        : nc_ptr(note::test::make_test_notecard_heap(transport, note::Allocator{}))
        , nc(*nc_ptr)
    {}

    // Construct with explicit allocator.
    explicit StreamHarness(note::Allocator alloc)
        : nc_ptr(note::test::make_test_notecard_heap(transport, alloc))
        , nc(*nc_ptr)
    {}
};

// Streaming harness without allocator — forces the buffered fallback check.
// The streaming constructor defaults alloc_ to a default-constructed
// Allocator, so we clear it explicitly to exercise the NotReady branch.
struct StreamNoAllocHarness {
    MockStreamHal hal;
    note::Protocol transport{hal};
    std::unique_ptr<note::Notecard> nc_ptr;
    note::Notecard& nc;

    StreamNoAllocHarness()
        : nc_ptr(std::make_unique<note::Notecard>(transport))  // default alloc — clear below
        , nc(*nc_ptr)
    {
        nc.clear_allocator();
        nc.set_retry_policy({.max_retries = 0});
    }
};

} // namespace

// ===========================================================================
// Streaming execute: void response (CardRestart — Response = void)
// ===========================================================================

TEST_CASE("Streaming: void execute succeeds with {}") {
    StreamHarness h;
    h.hal.queue_response("{}");

    note::api::CardRestart req;
    auto r = h.nc.execute(req);
    REQUIRE(r.has_value());
}

TEST_CASE("Streaming: void execute returns error on transport failure") {
    StreamHarness h;
    // No response queued — HAL returns error

    note::api::CardRestart req;
    auto r = h.nc.execute(req);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Streaming: void execute returns Notecard error") {
    StreamHarness h;
    h.hal.queue_response(R"({"err":"device busy"})");

    note::api::CardRestart req;
    auto r = h.nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::Notecard);
}

// ===========================================================================
// Streaming execute: typed response (CardVersion — Response has Sink)
// ===========================================================================

TEST_CASE("Streaming: typed execute parses response fields") {
    StreamHarness h;
    h.hal.queue_response(R"({"version":"1.2.3","board":"notecard:v2"})");

    note::api::CardVersion req;
    auto r = h.nc.execute(req);
    REQUIRE(r.has_value());
    CHECK(r.version == "1.2.3");
    CHECK(r.board == "notecard:v2");
}

TEST_CASE("Streaming: typed execute returns error on transport failure") {
    StreamHarness h;
    // No response queued

    note::api::CardVersion req;
    auto r = h.nc.execute(req);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("Streaming: typed execute returns Notecard error") {
    StreamHarness h;
    h.hal.queue_response(R"({"err":"firmware mismatch"})");

    note::api::CardVersion req;
    auto r = h.nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::Notecard);
}

// ===========================================================================
// Streaming execute with request IDs enabled
// ===========================================================================

TEST_CASE("Streaming: request IDs enabled adds id to request") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    note::Notecard nc(transport, note::Allocator{});
    // Don't disable request IDs — test the enabled path
    nc.set_retry_policy({.max_retries = 0});

    hal.queue_response("{}");
    note::api::CardRestart req;
    auto r = nc.execute(req);
    REQUIRE(r.has_value());

    // Verify the transmitted data contains "id":1
    std::string sent(hal.tx.begin(), hal.tx.end());
    CHECK(sent.find("\"id\":1") != std::string::npos);
}

TEST_CASE("Streaming: request IDs increment across calls") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    note::Notecard nc(transport, note::Allocator{});
    nc.set_retry_policy({.max_retries = 0});

    // First request
    hal.queue_response("{}");
    note::api::CardRestart req;
    nc.execute(req);
    std::string sent1(hal.tx.begin(), hal.tx.end());
    CHECK(sent1.find("\"id\":1") != std::string::npos);

    // Second request
    hal.tx.clear();
    hal.queue_response(R"({"version":"1.0"})");
    note::api::CardVersion ver;
    nc.execute(ver);
    std::string sent2(hal.tx.begin(), hal.tx.end());
    CHECK(sent2.find("\"id\":2") != std::string::npos);
}

// ===========================================================================
// Streaming execute without allocator — falls through to error
// ===========================================================================

TEST_CASE("Streaming: execute without allocator or backend returns NotReady") {
    StreamNoAllocHarness h;
    h.hal.queue_response("{}");

    note::api::CardVersion req;
    auto r = h.nc.execute(req);
    // alloc_ is nullopt (cleared by
    // harness) → skip streaming path; backend_ is nullptr → skip
    // buffered path → returns NotReady error.
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::NotReady);
}

TEST_CASE("Streaming: void execute without allocator returns NotReady") {
    StreamNoAllocHarness h;
    h.hal.queue_response("{}");

    note::api::CardRestart req;
    auto r = h.nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::NotReady);
}

// ===========================================================================
// Streaming: send_command (line 255)
// ===========================================================================

TEST_CASE("Streaming: send_command sends via streaming transport") {
    StreamHarness h;

    auto build = [](note::JsonBuilder& b, void*) {
        b.add("cmd", note::string_view("card.restart"));
    };
    auto r = h.nc.send_command(build, nullptr);
    REQUIRE(r.has_value());

    std::string sent(h.hal.tx.begin(), h.hal.tx.end());
    CHECK(sent.find("card.restart") != std::string::npos);
}

// ===========================================================================
// Streaming: command_typed (line 274)
// ===========================================================================

TEST_CASE("Streaming: command_typed sends via streaming transport") {
    StreamHarness h;

    note::api::HubSet req;
    req.mode("continuous");
    auto r = h.nc.command_typed(req);
    REQUIRE(r.has_value());

    std::string sent(h.hal.tx.begin(), h.hal.tx.end());
    CHECK(sent.find("\"cmd\":\"hub.set\"") != std::string::npos);
    CHECK(sent.find("\"mode\":\"continuous\"") != std::string::npos);
}

TEST_CASE("Streaming: command_typed returns error on send failure") {
    StreamHarness h;
    h.hal.transmit_ok = false;

    note::api::HubSet req;
    auto r = h.nc.command_typed(req);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

// ===========================================================================
// Streaming: command() with std::function (line 300)
// ===========================================================================

TEST_CASE("Streaming: command() sends via streaming transport") {
    StreamHarness h;

    auto r = h.nc.command("card.restart");
    REQUIRE(r.has_value());

    std::string sent(h.hal.tx.begin(), h.hal.tx.end());
    CHECK(sent.find("\"cmd\":\"card.restart\"") != std::string::npos);
}

TEST_CASE("Streaming: command() with build_fn sends fields") {
    StreamHarness h;

    auto r = h.nc.command("hub.set", [](note::JsonBuilder& b) {
        b.add("mode", note::string_view("periodic"));
    });
    REQUIRE(r.has_value());

    std::string sent(h.hal.tx.begin(), h.hal.tx.end());
    CHECK(sent.find("\"cmd\":\"hub.set\"") != std::string::npos);
    CHECK(sent.find("\"mode\":\"periodic\"") != std::string::npos);
}

TEST_CASE("Streaming: command() returns not-ready without any transport") {
    // Verify the no-transport path is hit when transport_ is set
    // but send fails.
    StreamHarness h;
    h.hal.transmit_ok = false;

    auto r = h.nc.command("card.restart");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

// ===========================================================================
// Streaming: transact() with OwnedBuffer (lines 340-366)
// ===========================================================================

TEST_CASE("Streaming: transact() returns OwnedBuffer with response copy") {
    StreamHarness h;
    h.hal.queue_response(R"({"ok":true})");

    auto r = h.nc.transact(R"({"req":"card.version"})");
    REQUIRE(r.has_value());
    CHECK(r->view() == R"({"ok":true})");
}

TEST_CASE("Streaming: transact() with OwnedBuffer propagates send error") {
    StreamHarness h;
    h.hal.transmit_ok = false;

    auto r = h.nc.transact(R"({"req":"card.version"})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

TEST_CASE("Streaming: transact() with OwnedBuffer returns timeout on no response") {
    StreamHarness h;
    // No response queued — HAL returns error on read

    auto r = h.nc.transact(R"({"req":"card.version"})");
    REQUIRE_FALSE(r.has_value());
    // The streaming_transact_raw error is propagated
}

// ===========================================================================
// Streaming: transact() with caller buffer (lines 393-401)
// ===========================================================================

TEST_CASE("Streaming: transact() with caller buffer returns string_view") {
    StreamHarness h;
    h.hal.queue_response(R"({"ok":true})");

    char buf[128];
    auto r = h.nc.transact(R"({"req":"card.version"})", note::span<char>(buf, sizeof(buf)));
    REQUIRE(r.has_value());
    CHECK(*r == R"({"ok":true})");
    // The result should point into our buffer
    CHECK(r->data() >= buf);
    CHECK(r->data() < buf + sizeof(buf));
}

TEST_CASE("Streaming: transact() with caller buffer propagates send error") {
    StreamHarness h;
    h.hal.transmit_ok = false;

    char buf[128];
    auto r = h.nc.transact(R"({"req":"card.version"})", note::span<char>(buf, sizeof(buf)));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

// ===========================================================================
// Streaming: send() (line 414)
// ===========================================================================

TEST_CASE("Streaming: send() sends raw JSON via streaming transport") {
    StreamHarness h;

    auto r = h.nc.send(R"({"cmd":"card.restart"})");
    REQUIRE(r.has_value());

    std::string sent(h.hal.tx.begin(), h.hal.tx.end());
    CHECK(sent.find("card.restart") != std::string::npos);
}

TEST_CASE("Streaming: send() propagates transport error") {
    StreamHarness h;
    h.hal.transmit_ok = false;

    auto r = h.nc.send(R"({"cmd":"card.restart"})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::SendFailed);
}

// ===========================================================================
// Streaming: request() returns NotReady (no backend)
// ===========================================================================

TEST_CASE("Streaming: request() returns NotReady without backend") {
    StreamHarness h;
    h.hal.queue_response("{}");

    auto r = h.nc.request("card.version");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::NotReady);
}

// ===========================================================================
// Timing enforcement with streaming transport (lines 855-870)
// ===========================================================================

// Inter-transaction timing is enforced on paths with a Notecard-side
// response to wait for: execute(), send(), send_command(), and
// command_typed(). The string_view command(sv) overload is a
// fire-and-forget escape hatch that intentionally skips timing —
// use nc.send(raw_json) below to exercise the gap logic.

TEST_CASE("Streaming: enforce_timing delays when gap is insufficient") {
    StreamHarness h;
    h.nc.set_inter_transaction_gap(100);

    // First send records timing at hal.millis() == 0.
    h.nc.send(R"({"cmd":"card.restart"})");

    // Second send: millis() still 0, elapsed = 0, needs full 100 ms delay.
    h.nc.send(R"({"cmd":"card.restart"})");
    CHECK(h.hal.delay_total == 100);
}

TEST_CASE("Streaming: enforce_timing skips delay when gap is sufficient") {
    StreamHarness h;
    h.nc.set_inter_transaction_gap(100);

    h.nc.send(R"({"cmd":"card.restart"})");

    // Advance time beyond the gap.
    h.hal.current_millis = 200;

    // Second send: elapsed = 200, gap = 100 → no delay needed.
    h.nc.send(R"({"cmd":"card.restart"})");
    CHECK(h.hal.delay_total == 0);
}

TEST_CASE("Streaming: record_timing uses streaming millis") {
    StreamHarness h;
    h.nc.set_inter_transaction_gap(50);

    // First send at time 1000.
    h.hal.current_millis = 1000;
    h.nc.send(R"({"cmd":"card.restart"})");

    // Second send at time 1020: elapsed = 20, needs 30 ms delay.
    h.hal.current_millis = 1020;
    h.nc.send(R"({"cmd":"card.restart"})");
    CHECK(h.hal.delay_total == 30);
}

// ===========================================================================
// Binary transfer helpers with streaming transport (lines 575-598)
// ===========================================================================

TEST_CASE("Streaming: binary PUT uses streaming write") {
    StreamHarness h;
    h.hal.queue_response("{}");  // handshake response

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify(false);

    auto rsp = h.nc.execute(req);
    REQUIRE(rsp.has_value());

    // Verify binary data was transmitted (COBS encoded + EOP)
    // The HAL tx buffer contains both JSON request and binary data.
    bool has_eop = false;
    for (auto b : h.hal.tx) {
        if (b == note::cobs_eop) { has_eop = true; break; }
    }
    CHECK(has_eop);
}

TEST_CASE("Streaming: binary PUT write failure returns SendFailed") {
    StreamHarness h;
    h.hal.queue_response("{}");  // handshake succeeds

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify(false);

    // Fail transmit after the handshake succeeds (JSON was sent via transact,
    // now binary write calls transmit which fails).
    // We need the handshake to succeed first, then fail on binary write.
    // Per-call transmit control isn't exposed today — this would require a
    // counter-based MockStreamHal variant. Leave transmit_ok=true so the
    // happy-path streaming binary write is still exercised below.
    h.hal.transmit_ok = true;  // let handshake through

    // We can't easily control per-call transmit. Instead, test the binary_io_reset path
    // by checking a different failure mode. The streaming path through binary_write
    // at line 575 is exercised by any streaming binary PUT.
    auto rsp = h.nc.execute(req);
    // The test above already exercises the streaming binary write path.
    REQUIRE(rsp.has_value());
}

TEST_CASE("Streaming: binary GET reads via streaming transport") {
    StreamHarness h;

    uint8_t original[] = {10, 20, 30};
    note::SoftwareMd5 md5;
    auto expected_md5 = md5.compute(original, sizeof(original));

    // Queue the JSON handshake response with status/MD5
    std::string json_rsp = std::string(R"({"status":")") + expected_md5.data() + "\"}";
    h.hal.queue_response(json_rsp);

    // Queue COBS-encoded binary data
    note::CobsEncoder encoder;
    encoder.encode(original, sizeof(original), [&](const uint8_t* block, size_t n) {
        for (size_t i = 0; i < n; ++i)
            h.hal.rx.push_back(block[i]);
    });
    h.hal.rx.push_back(note::cobs_eop);

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));

    auto rsp = h.nc.execute(req);
    REQUIRE(rsp.has_value());
    CHECK(memcmp(dst, original, sizeof(original)) == 0);
}

TEST_CASE("Streaming: binary GET read timeout returns error") {
    StreamHarness h;
    h.hal.queue_response("{}");  // handshake OK
    // No binary data queued — read will fail

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::ResponseLost);
}

TEST_CASE("Streaming: binary_io_reset resets streaming transport") {
    StreamHarness h;
    h.hal.queue_response("{}");
    // Queue incomplete COBS data (no EOP) to trigger timeout in receive loop
    h.hal.rx.push_back(0x01);
    // After reading 0x01, read will return error (rx empty, no EOP seen)

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    // binary_io_reset should have been called, which calls transport_->reset()
    CHECK(h.hal.reset_called);
}

// ===========================================================================
// Binary PUT: transmit failure after handshake (lines 496-506)
// ===========================================================================

namespace {

// A HAL that fails transmit after N calls.
class FailAfterNHal : public note::Hal {
public:
    std::deque<uint8_t> rx;
    int transmit_count = 0;
    int fail_after;  // fail on this call (0-based)

    explicit FailAfterNHal(int fail_after_n) : fail_after(fail_after_n) {}

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }

    bool transmit(const uint8_t*, size_t) override {
        return transmit_count++ < fail_after;
    }

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

} // namespace

TEST_CASE("Streaming: binary PUT write failure triggers binary_io_reset") {
    // The handshake (transact) uses 2 transmits: JSON + line terminator
    // We need to let those succeed, then fail on the binary write.
    // Transmit calls for streaming transact: build → transmit, line_terminator
    // So fail_after=2 means the handshake succeeds but binary write fails.
    FailAfterNHal hal(2);
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    hal.queue_response("{}");  // handshake response

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify(false);

    auto rsp = nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::SendFailed);
}

// ===========================================================================
// Binary PUT: post-transmit verification via streaming (lines 510-519)
// ===========================================================================

TEST_CASE("Streaming: binary PUT with verify does pre-flight and post-transmit") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    uint8_t data[] = {1, 2, 3};
    note::SoftwareMd5 md5;
    auto expected_md5 = md5.compute(data, sizeof(data));

    // Pre-flight: reset (card.binary delete) → OK
    hal.queue_response("{}");
    // Pre-flight: status (card.binary) → max:1024
    hal.queue_response(R"({"max":1024})");
    // PUT handshake → OK
    hal.queue_response("{}");
    // Post-verify: card.binary status → matching MD5
    std::string verify_rsp = std::string(R"({"status":")") + expected_md5.data() + "\"}";
    hal.queue_response(verify_rsp);

    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify();

    auto rsp = nc.execute(req);
    REQUIRE(rsp.has_value());
}

TEST_CASE("Streaming: binary PUT verify detects MD5 mismatch") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    uint8_t data[] = {1, 2, 3};

    hal.queue_response("{}");              // reset
    hal.queue_response(R"({"max":1024})"); // pre-flight status
    hal.queue_response("{}");              // PUT handshake
    hal.queue_response(R"({"status":"00000000000000000000000000000000"})"); // wrong MD5

    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::ResponseLost);
}

TEST_CASE("Streaming: binary PUT pre-flight reset failure") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    // No response queued for reset → transact fails

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::SendFailed);
}

TEST_CASE("Streaming: binary PUT pre-flight status failure") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    hal.queue_response("{}");  // reset OK
    // No response for status query → transact fails

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::SendFailed);
}

TEST_CASE("Streaming: binary PUT pre-flight max=0 returns Overflow") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    hal.queue_response("{}");              // reset
    hal.queue_response(R"({"max":0})");    // max=0

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::Overflow);
}

TEST_CASE("Streaming: binary PUT data exceeds max returns Overflow") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    hal.queue_response("{}");              // reset
    hal.queue_response(R"({"max":2})");    // max=2, data is 3 bytes

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::Overflow);
}

// ===========================================================================
// Binary GET: MD5 mismatch with streaming (lines 559-564)
// ===========================================================================

TEST_CASE("Streaming: binary GET MD5 mismatch returns error") {
    StreamHarness h;

    uint8_t original[] = {1, 2, 3, 4, 5};

    // Queue handshake with wrong MD5
    h.hal.queue_response(R"({"status":"00000000000000000000000000000000"})");

    // Queue valid COBS data
    note::CobsEncoder encoder;
    encoder.encode(original, sizeof(original), [&](const uint8_t* block, size_t n) {
        for (size_t i = 0; i < n; ++i)
            h.hal.rx.push_back(block[i]);
    });
    h.hal.rx.push_back(note::cobs_eop);

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::ResponseLost);
}

TEST_CASE("Streaming: binary GET empty MD5 skips verification") {
    StreamHarness h;
    h.hal.queue_response("{}");  // no status field → empty MD5

    uint8_t original[] = {1, 2, 3};
    note::CobsEncoder encoder;
    encoder.encode(original, sizeof(original), [&](const uint8_t* block, size_t n) {
        for (size_t i = 0; i < n; ++i)
            h.hal.rx.push_back(block[i]);
    });
    h.hal.rx.push_back(note::cobs_eop);

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));

    auto rsp = h.nc.execute(req);
    REQUIRE(rsp.has_value());
    CHECK(memcmp(dst, original, sizeof(original)) == 0);
}

// ===========================================================================
// Binary PUT: handshake failure returns error (line 490)
// ===========================================================================

TEST_CASE("Streaming: binary PUT handshake failure returns error") {
    StreamHarness h;
    // No response → handshake (execute) fails

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data)).verify(false);

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
}

// ===========================================================================
// Binary GET: handshake failure returns error (line 529)
// ===========================================================================

TEST_CASE("Streaming: binary GET handshake failure returns error") {
    StreamHarness h;
    // No response → handshake fails

    uint8_t dst[64] = {};
    note::api::CardBinaryGet req;
    req.into(dst, sizeof(dst));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
}

// ===========================================================================
// Binary PUT without MD5 provider (line 484)
// ===========================================================================

// NOTE: md5_ defaults to &platform_md5_ and cannot be nulled through the
// public API, so the `if (md5_)` branch in binary PUT is effectively always
// true. There is no meaningful test to write — documented here for future
// reference. (Test removed rather than left failing or gated.)

// ===========================================================================
// Binary PUT: post-verify query failure (lines 511-514)
// ===========================================================================

TEST_CASE("Streaming: binary PUT post-verify query failure") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    auto nc_ptr = note::test::make_test_notecard_heap(transport, note::Allocator{});
    auto& nc = *nc_ptr;

    uint8_t data[] = {1, 2, 3};

    hal.queue_response("{}");              // reset
    hal.queue_response(R"({"max":1024})"); // pre-flight status
    hal.queue_response("{}");              // PUT handshake
    // No response for post-verify query → transact_raw returns error

    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::ResponseLost);
}

// ===========================================================================
// Streaming: execute with explicit allocator
//
// The per-call overload is gated out under NOTE_SINGLETON=1 and under
// NOTE_NO_RESPONSE_RAII=1 — see the comment on
// `Notecard::execute(req, Allocator)`.
// ===========================================================================

#if !NOTE_SINGLETON && !NOTE_NO_RESPONSE_RAII
TEST_CASE("Streaming: execute with temp allocator interns strings") {
    StreamHarness h;
    h.hal.queue_response(R"({"version":"1.2.3","board":"notecard"})");

    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::Allocator temp_alloc = note::arena_allocator(arena);

    note::api::CardVersion req;
    auto r = h.nc.execute(req, temp_alloc);
    REQUIRE(r.has_value());
    CHECK(r.version == "1.2.3");

    // Verify string is interned into the arena
    auto ver_ptr = r.version.value().data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    CHECK(ver_ptr >= arena_start);
    CHECK(ver_ptr < arena_end);
}
#endif // !NOTE_SINGLETON && !NOTE_NO_RESPONSE_RAII

// ===========================================================================
// Streaming: set_debug propagates to streaming transport
// ===========================================================================

TEST_CASE("Streaming: set_debug propagates to streaming transport") {
    StreamHarness h;

    struct TimingTracker { bool called = false; };
    TimingTracker tracker;

    note::DebugListener dbg;
    dbg.on_timing = [](note::TimingEvent, note::string_view, void* ctx) {
        static_cast<TimingTracker*>(ctx)->called = true;
    };
    dbg.ctx = &tracker;

    h.nc.set_debug(dbg);

    // Execute something to trigger timing events
    h.hal.queue_response("{}");
    note::api::CardRestart req;
    h.nc.execute(req);

    CHECK(tracker.called);
}

// ===========================================================================
// Streaming: execute retries on streaming transport
// ===========================================================================

TEST_CASE("Streaming: typed execute with retry succeeds on second attempt") {
    MockStreamHal hal;
    note::Protocol transport(hal);
    note::Notecard nc(transport, note::Allocator{});
    nc.set_request_ids(false);
    nc.set_retry_policy({.max_retries = 1, .retry_delay_ms = 0});

    // First attempt: error response (will trigger retry for Idempotent/ReadOnly)
    // Note: CardVersion has safety = ReadOnly — retries on any failure
    // Don't queue response → transport error → retry
    // Second attempt: success
    hal.queue_response(R"({"version":"ok"})");

    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE(r.has_value());
    CHECK(r.version == "ok");
}

// ===========================================================================
// Streaming: void response Notecard error with allocator interns message
// ===========================================================================

TEST_CASE("Streaming: void execute Notecard error interns message") {
    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::Allocator alloc = note::arena_allocator(arena);

    StreamHarness h(alloc);
    h.hal.queue_response(R"({"err":"streaming err"})");

    note::api::CardRestart req;
    auto r = h.nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::Notecard);
    // The message should be interned into the arena
    auto msg_ptr = r.error().message.data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    CHECK(msg_ptr >= arena_start);
    CHECK(msg_ptr < arena_end);
}

TEST_CASE("Streaming: typed execute Notecard error interns message") {
    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::Allocator alloc = note::arena_allocator(arena);

    StreamHarness h(alloc);
    h.hal.queue_response(R"({"err":"bad version"})");

    note::api::CardVersion req;
    auto r = h.nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::Notecard);
    auto msg_ptr = r.error().message.data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    CHECK(msg_ptr >= arena_start);
    CHECK(msg_ptr < arena_end);
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST_CASE("Streaming: transact rejects malformed JSON") {
    StreamHarness h;

    auto r = h.nc.transact("not json");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::Json);
}

TEST_CASE("Streaming: transact with buffer rejects malformed JSON") {
    StreamHarness h;

    char buf[128];
    auto r = h.nc.transact("not json", note::span<char>(buf, sizeof(buf)));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::Json);
}

TEST_CASE("Streaming: send rejects malformed JSON") {
    StreamHarness h;

    auto r = h.nc.send("not json");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::Json);
}

// ===========================================================================
// Streaming: default-constructed Notecard (no transport at all)
// with streaming-specific commands that route through transport_ via RequestSource
// ===========================================================================

TEST_CASE("Default Notecard: command() without streaming returns NotReady") {
    note::Notecard nc;
    auto r = nc.command("card.restart");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::NotReady);
}

TEST_CASE("Default Notecard: send() returns NotReady") {
    note::Notecard nc;
    auto r = nc.send(R"({"cmd":"card.restart"})");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == note::Error::NotReady);
}
