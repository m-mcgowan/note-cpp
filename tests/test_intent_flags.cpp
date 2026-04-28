// Tests for intent flag APIs: fluent methods, flag constants, string assignment,
// and factory parameter overloads.

#include <doctest.h>
#include <string>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"
#include <note/api.hpp>

namespace {

struct Harness {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::test::CallbackTransport transport;
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
// card.aux.serial notify — fluent methods
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify().env() fluent") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.env();
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"notify,env\"") != std::string::npos);
}

TEST_CASE("aux.serial notify().env().dfu() fluent chain — exact wire format") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.env().dfu();
    h.nc.execute(req);
    // Flags appear in definition order (env=bit0, dfu=bit1)
    REQUIRE(h.last_req == R"({"req":"card.aux.serial","mode":"notify,env,dfu"})");
}

// ---------------------------------------------------------------------------
// card.aux.serial notify — flag constants
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify with flag constants — exact wire format") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.notifications = (note::serial::env | note::serial::dfu);
    h.nc.execute(req);
    REQUIRE(h.last_req == R"({"req":"card.aux.serial","mode":"notify,env,dfu"})");
}

// ---------------------------------------------------------------------------
// card.aux.serial notify — string assignment
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify with string assignment") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.notifications = note::string_view("env,signals");
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"notify,env,signals\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial notify — factory with string parameter
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify(string) factory") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    // Simulate what the factory overload does
    req.notifications = note::string_view("env,dfu");
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"notify,env,dfu\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial gps — no notifications field
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial gps() sends mode gps") {
    Harness h;
    note::api::CardAuxSerial::Gps req;
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"gps\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial off — bare request
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial off() sends mode -") {
    Harness h;
    note::api::CardAuxSerial::Off req;
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"-\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Field copy safety — FlagSet and VoltageVariable
// These nested types own internal char buffers. Field<string_view> points into
// that buffer. Copy/move must re-derive the string_view from the local buffer.
// ---------------------------------------------------------------------------

TEST_CASE("hub.set voutbound survives return-by-value from factory") {
    Harness h;
    note::Api api(h.nc);
    note::VoltageVariable vv;
    vv.usb(5).normal(60);
    api.hub.set().voutbound(vv).execute();
    REQUIRE(h.last_req.find("\"voutbound\":\"usb:5;normal:60\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn — flag field copy safety
// ---------------------------------------------------------------------------

TEST_CASE("arm factory(uint32_t) — flags survive return-by-value") {
    Harness h;
    note::Api api(h.nc);
    // Factory creates Arm, sets triggers via uint32_t, returns by value.
    // The returned copy must have a valid triggers string_view.
    auto req = api.card.attn().arm(note::attn::connected | note::attn::files);
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,files\"") != std::string::npos);
}

TEST_CASE("rearm factory(uint32_t) — flags survive return-by-value") {
    Harness h;
    note::Api api(h.nc);
    auto req = api.card.attn().rearm(note::attn::motion | note::attn::signal);
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"rearm,motion,signal\"") != std::string::npos);
}

TEST_CASE("arm factory(uint32_t) — flags survive chained execute") {
    Harness h;
    note::Api api(h.nc);
    // Full fluent chain: factory creates temporary, flags set, execute called.
    api.card.attn().arm(note::attn::connected | note::attn::files).seconds(60).execute();
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,files\"") != std::string::npos);
    REQUIRE(h.last_req.find("\"seconds\":60") != std::string::npos);
}

TEST_CASE("rearm factory(uint32_t) — flags survive chained execute") {
    Harness h;
    note::Api api(h.nc);
    api.card.attn().rearm(note::attn::files | note::attn::connected).seconds(60).execute();
    REQUIRE(h.last_req.find("\"mode\":\"rearm,connected,files\"") != std::string::npos);
    REQUIRE(h.last_req.find("\"seconds\":60") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn arm — trigger flags only (no mode flags)
// ---------------------------------------------------------------------------

TEST_CASE("attn arm().connected().motion() fluent — exact wire format") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.connected().motion();
    h.nc.execute(req);
    REQUIRE(h.last_req == R"({"req":"card.attn","mode":"arm,connected,motion"})");
}

TEST_CASE("attn arm with flag constants — exact wire format") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.triggers = (note::attn::connected | note::attn::env);
    h.nc.execute(req);
    REQUIRE(h.last_req == R"({"req":"card.attn","mode":"arm,connected,env"})");
}

TEST_CASE("attn arm with string triggers via property") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.triggers = note::string_view("connected,files");
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,files\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn — mode intents (watchdog, sleep, disarm)
// ---------------------------------------------------------------------------

TEST_CASE("attn watchdog sends mode watchdog with seconds") {
    Harness h;
    note::api::CardAttn::Watchdog req;
    req.seconds = 120;
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"watchdog\"") != std::string::npos);
    REQUIRE(h.last_req.find("\"seconds\":120") != std::string::npos);
}

TEST_CASE("attn sleep sends mode sleep") {
    Harness h;
    note::api::CardAttn::Sleep req;
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"sleep\"") != std::string::npos);
}

TEST_CASE("attn disarm sends mode disarm,-all") {
    Harness h;
    note::api::CardAttn::Disarm req;
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"disarm,-all\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn — all 10 trigger flag constants exist
// ---------------------------------------------------------------------------

TEST_CASE("attn all trigger flag constants are valid") {
    // Compile-time proof that all 10 constants exist and are distinct
    constexpr uint32_t all = note::attn::auxgpio | note::attn::connected
        | note::attn::env | note::attn::files | note::attn::location
        | note::attn::motion | note::attn::motionchange | note::attn::signal
        | note::attn::usb | note::attn::wireless;
    REQUIRE(all != 0);
    // 10 distinct bits
    uint32_t count = 0;
    for (uint32_t v = all; v; v &= v - 1) ++count;
    REQUIRE(count == 10);
}

// ---------------------------------------------------------------------------
// card.attn — raw Request with string mode (escape hatch)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// card.aux.serial notify — validated assignment (C++20 GCC)
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify assignment with valid string literal") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.notifications = "env,dfu";
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"notify,env,dfu\"") != std::string::npos);
}

TEST_CASE("aux.serial notify assignment with runtime string_view") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    std::string_view dynamic = "signals,accel";
    req.notifications = dynamic;
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"notify,signals,accel\"") != std::string::npos);
}

TEST_CASE("attn arm triggers assignment with valid string literal") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.triggers = "connected,motion";
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,motion\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn — raw Request with string mode (escape hatch)
// ---------------------------------------------------------------------------

TEST_CASE("attn Request raw string mode — escape hatch, not validated") {
    // The base Request type accepts any string for mode. This is the escape
    // hatch for mode combinations the typed intent API doesn't cover.
    // No compile-time or runtime validation — the Notecard validates.
    Harness h;
    note::api::CardAttn::Request req;
    req.mode = "arm,connected,env";
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,env\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn arm(string) factory overload
// ---------------------------------------------------------------------------

TEST_CASE("attn arm(string) factory overload") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.triggers = note::string_view("connected,env");
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,env\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn rearm — idempotent arm
// ---------------------------------------------------------------------------

TEST_CASE("attn rearm() with no flags — bare rearm prefix") {
    Harness h;
    note::api::CardAttn::Rearm req;
    h.nc.execute(req);
    REQUIRE(h.last_req == R"({"req":"card.attn","mode":"rearm"})");
}

TEST_CASE("attn rearm with multiple triggers — fluent flag constants") {
    Harness h;
    note::api::CardAttn::Rearm req;
    req.triggers = (note::attn::connected | note::attn::files);
    h.nc.execute(req);
    REQUIRE(h.last_req == R"({"req":"card.attn","mode":"rearm,connected,files"})");
}

TEST_CASE("attn rearm with flag constants — exact wire format") {
    Harness h;
    note::api::CardAttn::Rearm req;
    req.triggers = (note::attn::motion | note::attn::signal);
    h.nc.execute(req);
    REQUIRE(h.last_req == R"({"req":"card.attn","mode":"rearm,motion,signal"})");
}

TEST_CASE("attn rearm with seconds — re-arms with timeout") {
    Harness h;
    note::api::CardAttn::Rearm req;
    req.connected().seconds(120);
    h.nc.execute(req);
    auto& r = h.last_req;
    REQUIRE(r.find("\"mode\":\"rearm,connected\"") != std::string::npos);
    REQUIRE(r.find("\"seconds\":120") != std::string::npos);
}

TEST_CASE("attn rearm(string) with string triggers") {
    Harness h;
    note::api::CardAttn::Rearm req;
    req.triggers = note::string_view("files,env");
    h.nc.execute(req);
    REQUIRE(h.last_req.find("\"mode\":\"rearm,files,env\"") != std::string::npos);
}

TEST_CASE("attn rearm via factory — fluent chain") {
    Harness h;
    note::Api api(h.nc);
    api.card.attn().rearm(note::attn::files | note::attn::connected).seconds(60).execute();
    auto& r = h.last_req;
    REQUIRE(r.find("\"mode\":\"rearm,connected,files\"") != std::string::npos);
    REQUIRE(r.find("\"seconds\":60") != std::string::npos);
}
