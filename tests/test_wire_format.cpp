// Wire format tests: verify that generated request types produce correct JSON.
#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/notecard.hpp>
#include <note/api_context.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_get.hpp>
#include <note/api/note_add.hpp>
#include <note/api/env_set.hpp>
#include <note/api/card_binary_get.hpp>
#include <note/api/card_binary.hpp>

namespace {

// Helper to execute a request and capture the JSON
struct TestHarness {
    note::test::TestJsonBackend backend;
    std::string last_request;
    note::Notecard nc;

    TestHarness() : nc(backend,
        [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            last_request = std::string(req);
            return std::string("{}");
        },
        [this](note::string_view req) -> note::Result<void> {
            last_request = std::string(req);
            return {};
        }) {}
};

} // namespace

// ---------------------------------------------------------------------------
// Simple endpoints
// ---------------------------------------------------------------------------

TEST_CASE("CardVersion produces minimal request") {
    TestHarness h;
    note::api::CardVersion req;
    h.nc.execute(req);
    REQUIRE(h.last_request == R"({"req":"card.version"})");
}

TEST_CASE("HubSet with product and mode") {
    TestHarness h;
    note::api::HubSet req;
    req.product("com.example.test").mode("periodic");
    h.nc.execute(req);
    // Fields emitted in schema order (alphabetical): mode before product
    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"periodic","product":"com.example.test"})");
}

TEST_CASE("HubSet with no fields emits only req") {
    TestHarness h;
    note::api::HubSet req;
    h.nc.execute(req);
    REQUIRE(h.last_request == R"({"req":"hub.set"})");
}

TEST_CASE("HubSet with integer field") {
    TestHarness h;
    note::api::HubSet req;
    req.outbound(60);
    h.nc.execute(req);
    REQUIRE(h.last_request == R"({"req":"hub.set","outbound":60})");
}

TEST_CASE("EnvSet with string fields") {
    TestHarness h;
    note::api::EnvSet req;
    req.name = "temperature";
    req.text("22.5");
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"env.set","name":"temperature","text":"22.5"})");
}

// ---------------------------------------------------------------------------
// Polymorphic endpoints (dispatch)
// ---------------------------------------------------------------------------

TEST_CASE("NoteGet::Get excludes delete property") {
    TestHarness h;
    note::api::NoteGet::Get req;
    req.file("data.qi");
    h.nc.execute(req);
    // Should NOT contain "delete"
    REQUIRE(h.last_request == R"({"req":"note.get","file":"data.qi"})");
}

TEST_CASE("NoteGet::Delete includes delete:true") {
    TestHarness h;
    note::api::NoteGet::Delete req;
    req.file("requests.qi");
    h.nc.execute(req);
    // "delete":true should appear (required by dispatch)
    REQUIRE(h.last_request ==
        R"({"req":"note.get","delete":true,"file":"requests.qi"})");
}

TEST_CASE("NoteGet::Get with note_id uses wire name 'note'") {
    TestHarness h;
    note::api::NoteGet::Get req;
    req.file("settings.db").noteId("config");
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"note.get","file":"settings.db","note":"config"})");
}

// ---------------------------------------------------------------------------
// Safety metadata
// ---------------------------------------------------------------------------

TEST_CASE("Safety levels are correct") {
    STATIC_REQUIRE(note::api::CardVersion::safety == note::Safety::ReadOnly);
    STATIC_REQUIRE(note::api::HubSet::safety == note::Safety::Idempotent);
    STATIC_REQUIRE(note::api::NoteGet::Get::safety == note::Safety::ReadOnly);
    STATIC_REQUIRE(note::api::NoteGet::Delete::safety == note::Safety::Destructive);
    STATIC_REQUIRE(note::api::CardBinaryGet::safety == note::Safety::NonIdempotent);
}

TEST_CASE("supports_cmd is correct") {
    STATIC_REQUIRE(note::api::CardVersion::supports_cmd == true);
    STATIC_REQUIRE(note::api::HubSet::supports_cmd == true);
}

TEST_CASE("notecard_request wire names are correct") {
    STATIC_REQUIRE(note::api::CardVersion::notecard_request == "card.version");
    STATIC_REQUIRE(note::api::NoteGet::Get::notecard_request == "note.get");
    STATIC_REQUIRE(note::api::NoteGet::Delete::notecard_request == "note.get");
}

// ---------------------------------------------------------------------------
// Binary transfer annotation
// ---------------------------------------------------------------------------

TEST_CASE("CardBinaryGet has binary transfer annotation") {
    STATIC_REQUIRE(note::api::CardBinaryGet::BinaryTransfer::direction == "receive");
    STATIC_REQUIRE(note::api::CardBinaryGet::BinaryTransfer::encoding == "cobs");
}

// ---------------------------------------------------------------------------
// Fluent setter chaining
// ---------------------------------------------------------------------------

TEST_CASE("Fluent setters chain correctly") {
    note::api::HubSet req;
    auto& ref = req.product("test").mode("periodic").outbound(30);
    REQUIRE(&ref == &req);
}

// ---------------------------------------------------------------------------
// Command (fire-and-forget) uses "cmd" instead of "req"
// ---------------------------------------------------------------------------

TEST_CASE("Command uses cmd key") {
    TestHarness h;
    auto result = h.nc.command("hub.set", [](note::JsonBuilder& b) {
        b.add("product", "com.example.test");
    });
    REQUIRE(result);
    REQUIRE(h.last_request ==
        R"({"cmd":"hub.set","product":"com.example.test"})");
}

// ---------------------------------------------------------------------------
// API version gating
// ---------------------------------------------------------------------------

TEST_CASE("Version macro computes correctly") {
    STATIC_REQUIRE(NOTE_VERSION(9, 1, 1) == 90101);
    STATIC_REQUIRE(NOTE_VERSION(3, 2, 1) == 30201);
    STATIC_REQUIRE(NOTE_VERSION(5, 3, 1) > NOTE_VERSION(3, 4, 1));
}

// ---------------------------------------------------------------------------
// Field<T> wrapper
// ---------------------------------------------------------------------------

TEST_CASE("Field<T> implicit conversion for string_view") {
    note::Field<note::string_view> f;
    f = "hello";
    note::string_view sv = f;  // implicit conversion
    REQUIRE(sv == "hello");
}

TEST_CASE("Field<T> works as optional") {
    note::Field<int32_t> f;
    REQUIRE_FALSE(f.has_value());
    f = 42;
    REQUIRE(f.has_value());
    REQUIRE(*f == 42);
    f = std::nullopt;
    REQUIRE_FALSE(f.has_value());
}

TEST_CASE("Direct field assignment produces correct wire format") {
    TestHarness h;
    note::api::HubSet req;
    req.product = "com.example.test";
    req.mode = "periodic";
    h.nc.execute(req);
    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"periodic","product":"com.example.test"})");
}

// ---------------------------------------------------------------------------
// req.execute(nc) member method
// ---------------------------------------------------------------------------

TEST_CASE("req.execute(nc) works like nc.execute(req)") {
    TestHarness h;
    note::api::HubSet req;
    req.product("test");
    req.execute(h.nc);
    REQUIRE(h.last_request == R"({"req":"hub.set","product":"test"})");
}

TEST_CASE("Typed command_typed via req.command(nc)") {
    TestHarness h;
    note::api::HubSet req;
    req.product("test");
    req.command(h.nc);
    REQUIRE(h.last_request == R"({"cmd":"hub.set","product":"test"})");
}

// ---------------------------------------------------------------------------
// Api factory
// ---------------------------------------------------------------------------

TEST_CASE("Api factory creates bound requests") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.hubSet();
    req.product("factory-test");
    req.execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","product":"factory-test"})");
}

TEST_CASE("Api factory fluent chain") {
    TestHarness h;
    note::Api api(h.nc);
    api.hubSet().product("chain-test").mode("periodic").execute();
    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"periodic","product":"chain-test"})");
}

TEST_CASE("Api factory polymorphic endpoints") {
    TestHarness h;
    note::Api api(h.nc);
    api.noteGet().get().file("data.qi").execute();
    REQUIRE(h.last_request == R"({"req":"note.get","file":"data.qi"})");

    api.noteGet().delete_().file("data.qi").execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.get","delete":true,"file":"data.qi"})");
}

TEST_CASE("Api.execute with designated initializers") {
    TestHarness h;
    note::Api api(h.nc);
    api.execute(note::api::EnvSet{.name = "temp", .text = "22.5"});
    REQUIRE(h.last_request ==
        R"({"req":"env.set","name":"temp","text":"22.5"})");
}

TEST_CASE("Api.command sends cmd") {
    TestHarness h;
    note::Api api(h.nc);
    note::api::HubSet req;
    req.product("cmd-test");
    api.command(req);
    REQUIRE(h.last_request == R"({"cmd":"hub.set","product":"cmd-test"})");
}

// ---------------------------------------------------------------------------
// consteval enum validation
// ---------------------------------------------------------------------------

TEST_CASE("consteval validated_mode accepts valid values") {
    constexpr auto m = note::api::HubSet::validatedMode("periodic");
    STATIC_REQUIRE(m == "periodic");
    constexpr auto m2 = note::api::HubSet::validatedMode("continuous");
    STATIC_REQUIRE(m2 == "continuous");
}

// Note: invalid values produce compile errors — tested by attempting to build
// and verifying failure. Cannot test negative case at runtime.
