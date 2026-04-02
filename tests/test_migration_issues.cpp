// Tests for migration issues reported during note-c → note-cpp conversion.
// Each test documents a specific usability issue.

#include "catch.hpp"
#include "test_json_backend.hpp"
#include <note/api.hpp>
#include <note/notecard_api.hpp>
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
        , nc(backend, transport) {}
};

} // namespace

// ---------------------------------------------------------------------------
// Issue 1: begin() without allocator for streaming transport
// nc.begin(Serial1, 9600) should work with a default heap allocator.
// Currently only the buffered overload exists without an allocator.
// ---------------------------------------------------------------------------

TEST_CASE("Issue 1: NotecardApi::begin(IStreamingTransport&) without allocator") {
    // Streaming begin() should work without an explicit allocator (uses heap default).
    struct FakeStreamingTransport : note::IStreamingTransport {
        note::Result<void> transact(note::BuildFn, void*, note::JsonSink&, uint32_t) override { return {}; }
        note::Result<void> send(note::BuildFn, void*) override { return {}; }
        void reset() override {}
        void abort() override {}
    } transport;

    note::NotecardApi nc;
    nc.begin(transport);  // no allocator needed
    REQUIRE(true);
}

// ---------------------------------------------------------------------------
// Issue 2: "rearm" rejected by consteval mode validator on base Request
// ---------------------------------------------------------------------------

TEST_CASE("Issue 2: rearm as string on base Request type") {
    Harness h;
    note::api::CardAttn::Request req;
    // "rearm" is a valid mode value per the OpenAPI spec.
    // The consteval validator on mode_t should accept it.
    // Workaround: use note::string_view("rearm") to bypass consteval.
    req.mode = note::string_view("rearm");
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"rearm\"") != std::string::npos);
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
// Issue 6: raw JSON passthrough via transport
// ---------------------------------------------------------------------------

TEST_CASE("Issue 6: raw JSON passthrough via notecard().transport()") {
    Harness h;
    // Demonstrates the escape hatch for raw JSON
    h.nc.command("card.version", [](note::JsonBuilder&) {});
    // The command was sent through the transport
    REQUIRE(h.last_req.find("card.version") != std::string::npos);
}
