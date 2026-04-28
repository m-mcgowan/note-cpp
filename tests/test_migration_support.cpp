// Tests for migration issues reported during note-c → note-cpp conversion.
// Each test documents a specific usability issue.

#include <doctest.h>
#include <string>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"
#include <note/api.hpp>
#include <note/backends/buffer.hpp>
#include <note/notecard_api.hpp>
#include <note/streaming_transport.hpp>
#include <note/transport/serial.hpp>
#include <note/units.hpp>

namespace {

struct Harness {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport;
    note::Notecard nc;

    Harness()
        : transport(
            [this](note::string_view r, uint32_t) -> note::Result<note::string_view> {
                last_req = std::string(r);
                return note::string_view("{}");
            })
        , nc(note::test::make_test_notecard(backend, transport)) {}
};

} // namespace

// ---------------------------------------------------------------------------
// Issue 1: begin() without allocator for streaming transport
// nc.begin(Serial1, 9600) should work with a default heap allocator.
// Currently only the buffered overload exists without an allocator.
// ---------------------------------------------------------------------------

TEST_CASE("Issue 1: NotecardApi::begin(IStreamingTransport&) without allocator") {
    // Streaming begin() should work without an explicit allocator (uses heap default).
    struct FakeHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return note::Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    };
    struct FakeStreamingTransport : note::IStreamingTransport {
        FakeHal fake_hal;
        note::Result<void> transact(note::BuildFn, void*, note::JsonSink&, uint32_t) override { return {}; }
        note::Result<void> send(note::BuildFn, void*) override { return {}; }
        void reset() override {}
        void abort() override {}
        note::Hal& hal() override { return fake_hal; }
    } transport;

    note::NotecardApi nc;
    nc.begin(transport);  // no allocator needed
    REQUIRE(true);
}

// ---------------------------------------------------------------------------
// Issue 2: "rearm" rejected by consteval mode validator on base Request
// ---------------------------------------------------------------------------

TEST_CASE("Issue 2: rearm accepted by mode consteval validator") {
    Harness h;
    note::api::CardAttn::Request req;
    // "rearm" is now accepted by the consteval validator — no workaround needed.
    req.mode = "rearm";
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"rearm\"") != std::string::npos);
}

TEST_CASE("Issue 2: arm,connected,files accepted by mode validator") {
    Harness h;
    note::api::CardAttn::Request req;
    req.mode = "arm,connected,files";
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,files\"") != std::string::npos);
}

TEST_CASE("Issue 2: disarm,-all accepted by mode validator") {
    Harness h;
    note::api::CardAttn::Request req;
    req.mode = "disarm,-all";
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"disarm,-all\"") != std::string::npos);
}

TEST_CASE("Issue 2 resolved: use Rearm intent instead") {
    Harness h;
    note::Api api(h.nc);
    api.card.attn().rearm().execute();
    REQUIRE(h.last_req.find("\"mode\":\"rearm\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Issue 3: Rearm intent — RESOLVED by this session's commit
// ---------------------------------------------------------------------------

TEST_CASE("Issue 3 resolved: rearm() factory method exists") {
    Harness h;
    note::Api api(h.nc);
    api.card.attn().rearm(note::attn::files | note::attn::connected).seconds(60).execute();
    REQUIRE(h.last_req.find("\"mode\":\"rearm,connected,files\"") != std::string::npos);
    REQUIRE(h.last_req.find("\"seconds\":60") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Issue 4: off=true has no intent — requires base Request
// ---------------------------------------------------------------------------

TEST_CASE("Issue 4: off() intent disables ATTN processing") {
    Harness h;
    note::Api api(h.nc);
    api.card.attn().off().execute();
    REQUIRE(h.last_req.find("\"off\":true") != std::string::npos);
}

TEST_CASE("Issue 4: on() intent re-enables ATTN processing") {
    Harness h;
    note::Api api(h.nc);
    api.card.attn().on().execute();
    REQUIRE(h.last_req.find("\"on\":true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Issue 5: note::literals namespace reachable
// ---------------------------------------------------------------------------

TEST_CASE("Issue 5: note::literals are reachable") {
    using namespace note::literals;
    auto mins = 60_mins;
    auto hrs = 2_hours;
    auto secs = 30_s;
    REQUIRE(mins.count == 60);
    REQUIRE(hrs.count == 2);
    REQUIRE(secs.count == 30);
}

TEST_CASE("Issue 5: literals work with request fields") {
    Harness h;
    using namespace note::literals;
    note::Api api(h.nc);
    api.card.attn().arm(note::attn::files).seconds(120_s).execute();
    REQUIRE(h.last_req.find("\"seconds\":120") != std::string::npos);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// card.aux state: array of objects response
// ---------------------------------------------------------------------------

TEST_CASE("card.aux state: get_object_array reads pin states") {
    // Simulate a card.aux response with state array
    std::string canned = R"({"mode":"gpio","state":[{"high":true},{"low":true,"input":true},{}]})";
    note::backends::BufferJsonBackend<1024, 64> backend;
    note::CallbackTransport transport(
        [&](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view(canned);
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    // Parse response via the buffered path — uses real JSON parser
    auto rsp = nc.request("card.aux");
    REQUIRE(rsp);

    // Access the state array via the reader
    std::unique_ptr<note::JsonReader> pins[8];
    size_t n = (*rsp)->get_object_array("state", pins, 8);
    REQUIRE(n == 3);

    // Pin 0: high=true
    CHECK(pins[0]->get_bool("high") == true);
    CHECK(pins[0]->get_bool("low") == false);

    // Pin 1: low=true, input=true
    CHECK(pins[1]->get_bool("low") == true);
    CHECK(pins[1]->get_bool("input") == true);

    // Pin 2: empty object
    CHECK(pins[2]->get_bool("high") == false);
}

// ---------------------------------------------------------------------------
// Issue 1b: arduino::Notecard::begin() wiring
// The HAL (transport::NotecardSerial<>) must go through StreamingTransport
// before reaching NotecardApi::begin(IStreamingTransport&).
// ---------------------------------------------------------------------------

TEST_CASE("Issue 1b: transport::NotecardSerial is a Hal, not IStreamingTransport") {
    // Verify that the transport types are what we expect
    static_assert(!std::is_base_of_v<note::IStreamingTransport, note::transport::NotecardSerial<>>,
        "NotecardSerial should NOT implement IStreamingTransport directly");
    static_assert(std::is_base_of_v<note::Hal, note::transport::NotecardSerial<>>,
        "NotecardSerial should be a Hal");
    static_assert(std::is_base_of_v<note::IStreamingTransport, note::StreamingTransport>,
        "StreamingTransport should implement IStreamingTransport");
    REQUIRE(true);
}

// ---------------------------------------------------------------------------
// Issue 6: raw JSON passthrough
// The migration from note-c needs a way to send pre-formatted JSON strings,
// e.g. for serial passthrough protocols where a Python test script sends
// arbitrary JSON to the Notecard via the host firmware.
// ---------------------------------------------------------------------------

TEST_CASE("Issue 6: transact with allocator-managed buffer") {
    Harness h;
    // No buffer arg — uses the Notecard's allocator (heap by default)
    auto rsp = h.nc.transact(R"({"req":"card.version"})");
    REQUIRE(rsp);
    REQUIRE(h.last_req == R"({"req":"card.version"})");
}

TEST_CASE("Issue 6: transact with caller-provided buffer") {
    Harness h;
    char buf[256];
    auto rsp = h.nc.transact(R"({"req":"card.version"})", buf);
    REQUIRE(rsp);
    REQUIRE(h.last_req == R"({"req":"card.version"})");
}

TEST_CASE("Issue 6: send fire-and-forget") {
    Harness h;
    auto result = h.nc.send(R"({"cmd":"hub.set","product":"com.example"})");
    REQUIRE(result);
    REQUIRE(h.last_req == R"({"cmd":"hub.set","product":"com.example"})");
}

TEST_CASE("Issue 6: transact returns overflow error for large response") {
    // Streaming transport with a response larger than the buffer
    struct MockHal : note::Hal {
        std::string canned_rsp;
        size_t rsp_pos = 0;

        explicit MockHal(size_t rsp_size) : canned_rsp(rsp_size, 'x') {
            canned_rsp = "{\"data\":\"" + canned_rsp + "\"}\r\n";
        }
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t) override {
            if (rsp_pos >= canned_rsp.size()) return size_t(0);
            size_t n = std::min(max, canned_rsp.size() - rsp_pos);
            std::memcpy(buf, canned_rsp.data() + rsp_pos, n);
            rsp_pos += n;
            return n;
        }
        bool reset() override { return true; }
        bool write_line_terminator() override { rsp_pos = 0; return true; }
        void delay(uint32_t) override {}
        uint32_t millis() override { return 0; }
    };

    MockHal hal(200);  // response > 64 bytes
    note::StreamingTransport transport(hal);
    auto nc = note::test::make_test_notecard(transport);

    char buf[64];  // intentionally small
    auto rsp = nc.transact(R"({"req":"card.version"})", buf);
    REQUIRE(!rsp);  // must return error, not corrupted data
    REQUIRE(rsp.error().code == note::Error::Overflow);
}

TEST_CASE("Issue 6: transact rejects malformed JSON") {
    Harness h;
    char buf[256];
    auto rsp = h.nc.transact("not json at all", buf);
    REQUIRE(!rsp);
}

TEST_CASE("Issue 6: send rejects malformed JSON") {
    Harness h;
    auto result = h.nc.send("{missing closing brace");
    REQUIRE(!result);
}

// ---------------------------------------------------------------------------
// Issue 6b: streaming passthrough must preserve nested JSON
// The BufSink SAX re-serializes the response. Nested objects and arrays
// must not be flattened.
// ---------------------------------------------------------------------------

TEST_CASE("Issue 6b: buffered passthrough preserves nested objects") {
    // Buffered transport returns response verbatim — no re-serialization
    note::test::TestJsonBackend backend;
    std::string last_req;
    std::string canned_rsp = R"({"version":"7.2.1","body":{"org":"blues","product":"feather"}})";
    note::CallbackTransport transport(
        [&](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return note::string_view(canned_rsp);
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    char buf[512];
    auto rsp = nc.transact(R"({"req":"card.version"})", buf);
    REQUIRE(rsp);
    // Response must contain the nested body object, not flattened fields
    REQUIRE(std::string(*rsp).find("\"body\":{") != std::string::npos);
    REQUIRE(std::string(*rsp).find("\"org\":\"blues\"") != std::string::npos);
}

TEST_CASE("Issue 6b: streaming passthrough preserves nested objects") {
    // Mock HAL that returns a canned response with nested JSON.
    // The raw passthrough reads bytes directly — no SAX reconstruction.
    struct MockHal : note::Hal {
        std::string last_sent;
        std::string canned_rsp = "{\"version\":\"7.2.1\",\"body\":{\"org\":\"blues\",\"product\":\"feather\"}}\r\n";
        size_t rsp_pos = 0;

        bool transmit(const uint8_t* data, size_t len) override {
            last_sent.append(reinterpret_cast<const char*>(data), len);
            return true;
        }
        note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t) override {
            if (rsp_pos >= canned_rsp.size()) return size_t(0);
            size_t n = std::min(max, canned_rsp.size() - rsp_pos);
            std::memcpy(buf, canned_rsp.data() + rsp_pos, n);
            rsp_pos += n;
            return n;
        }
        bool reset() override { return true; }
        bool write_line_terminator() override {
            last_sent += "\r\n";
            rsp_pos = 0;  // reset for reading response
            return true;
        }
        void delay(uint32_t) override {}
        uint32_t millis() override { return 0; }
    };

    MockHal hal;
    note::StreamingTransport transport(hal);
    auto nc = note::test::make_test_notecard(transport, note::Allocator{});

    char buf[512];
    auto rsp = nc.transact(R"({"req":"card.version"})", buf);
    REQUIRE(rsp);
    auto rsp_str = std::string(*rsp);
    // Raw bytes preserved — nested body object intact
    REQUIRE(rsp_str.find("\"body\":{") != std::string::npos);
    REQUIRE(rsp_str.find("\"org\":\"blues\"") != std::string::npos);
}

TEST_CASE("Issue 6b: streaming passthrough preserves arrays") {
    struct MockHal : note::Hal {
        std::string canned_rsp = "{\"files\":[\"data.qi\",\"config.db\"],\"total\":2}\r\n";
        size_t rsp_pos = 0;

        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t) override {
            if (rsp_pos >= canned_rsp.size()) return size_t(0);
            size_t n = std::min(max, canned_rsp.size() - rsp_pos);
            std::memcpy(buf, canned_rsp.data() + rsp_pos, n);
            rsp_pos += n;
            return n;
        }
        bool reset() override { return true; }
        bool write_line_terminator() override { rsp_pos = 0; return true; }
        void delay(uint32_t) override {}
        uint32_t millis() override { return 0; }
    };

    MockHal hal;
    note::StreamingTransport transport(hal);
    auto nc = note::test::make_test_notecard(transport, note::Allocator{});

    char buf[512];
    auto rsp = nc.transact(R"({"req":"file.changes"})", buf);
    REQUIRE(rsp);
    auto rsp_str = std::string(*rsp);
    REQUIRE(rsp_str.find("\"files\":[") != std::string::npos);
    REQUIRE(rsp_str.find("\"data.qi\"") != std::string::npos);
    REQUIRE(rsp_str.find("\"config.db\"") != std::string::npos);
}
