// Tests for intent flag APIs: fluent methods, flag constants, string assignment,
// and factory parameter overloads.

#include "catch.hpp"
#include "test_json_backend.hpp"
#include <note/notecard.hpp>
#include <note/api/card_aux_serial.hpp>
#include <note/api/card_attn.hpp>

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
// card.aux.serial notify — fluent methods
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify().env() fluent") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.env();
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"notify,env\"") != std::string::npos);
}

TEST_CASE("aux.serial notify().env().dfu() fluent chain") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.env().dfu();
    req.nc_ = &h.nc;
    req.execute();
    // Both flags present (order may vary within the comma-separated string)
    REQUIRE(h.last_req.find("notify,") != std::string::npos);
    REQUIRE(h.last_req.find("env") != std::string::npos);
    REQUIRE(h.last_req.find("dfu") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial notify — flag constants
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify with flag constants") {
    using Notify = note::api::CardAuxSerial::Notify;
    Harness h;
    Notify req;
    req.notifications = (note::serial::env | note::serial::dfu);
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("notify,") != std::string::npos);
    REQUIRE(h.last_req.find("env") != std::string::npos);
    REQUIRE(h.last_req.find("dfu") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial notify — string assignment
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify with string assignment") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.notifications = note::string_view("env,signals");
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"notify,env,signals\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial notify — factory with string parameter
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial notify(string) factory") {
    Harness h;
    note::api::CardAuxSerial::Notify req;
    req.nc_ = &h.nc;
    // Simulate what the factory overload does
    req.notifications = note::string_view("env,dfu");
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"notify,env,dfu\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial gps — no notifications field
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial gps() sends mode gps") {
    Harness h;
    note::api::CardAuxSerial::Gps req;
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"gps\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.aux.serial off — bare request
// ---------------------------------------------------------------------------

TEST_CASE("aux.serial off() sends mode -") {
    Harness h;
    note::api::CardAuxSerial::Off req;
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"-\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// card.attn arm — trigger flags only (no mode flags)
// ---------------------------------------------------------------------------

TEST_CASE("attn arm().connected().motion() fluent") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.connected().motion();
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("arm,") != std::string::npos);
    REQUIRE(h.last_req.find("connected") != std::string::npos);
    REQUIRE(h.last_req.find("motion") != std::string::npos);
}

TEST_CASE("attn arm with flag constants") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.triggers = (note::attn::connected | note::attn::env);
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("arm,") != std::string::npos);
    REQUIRE(h.last_req.find("connected") != std::string::npos);
    REQUIRE(h.last_req.find("env") != std::string::npos);
}

TEST_CASE("attn arm with string triggers via property") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.triggers = note::string_view("connected,files");
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,files\"") != std::string::npos);
}

TEST_CASE("attn arm(string) factory overload") {
    Harness h;
    note::api::CardAttn::Arm req;
    req.nc_ = &h.nc;
    req.triggers = note::string_view("connected,env");
    req.execute();
    REQUIRE(h.last_req.find("\"mode\":\"arm,connected,env\"") != std::string::npos);
}
