// Wire format tests: verify that generated request types produce correct JSON.
#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/notecard.hpp>
#include <note/notecard_api.hpp>
#include <note/api.hpp>
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
    note::CallbackTransport transport;
    note::Notecard nc;

    TestHarness()
        : transport(
            [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                last_request = std::string(req);
                return note::string_view("{}");
            },
            [this](note::string_view req) -> note::Result<void> {
                last_request = std::string(req);
                return {};
            })
        , nc(backend, transport) {}
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
    STATIC_REQUIRE(note::api::CardBinaryGet::BinaryTransfer::direction == note::Direction::Receive);
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
    auto req = api.hub.set();
    req.product("factory-test");
    req.execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","product":"factory-test"})");
}

TEST_CASE("Api factory fluent chain") {
    TestHarness h;
    note::Api api(h.nc);
    api.hub.set().product("chain-test").mode("periodic").execute();
    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"periodic","product":"chain-test"})");
}

TEST_CASE("Api factory polymorphic endpoints") {
    TestHarness h;
    note::Api api(h.nc);
    api.note.get().read().file("data.qi").execute();
    REQUIRE(h.last_request == R"({"req":"note.get","file":"data.qi"})");

    api.note.get().pop().file("data.qi").execute();
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

// ---------------------------------------------------------------------------
// ArrayField wire format
// ---------------------------------------------------------------------------

TEST_CASE("ArrayField: add() serializes as JSON array") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.file.delete_();
    req.files.add("data.qi").add("settings.db");
    req.execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["data.qi","settings.db"]})");
}

TEST_CASE("ArrayField: initializer-list assignment") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.file.delete_();
    req.files = {"data.qi", "settings.db"};
    req.execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["data.qi","settings.db"]})");
}

TEST_CASE("ArrayField: operator() initializer-list") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.file.delete_();
    req.files({"data.qi", "settings.db"});
    req.execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["data.qi","settings.db"]})");
}

TEST_CASE("ArrayField: empty field not serialized") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.file.delete_();
    req.execute();
    REQUIRE(h.last_request == R"({"req":"file.delete"})");
}

TEST_CASE("ArrayField: single-value assignment") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.file.delete_();
    req.files = "data.qi";
    req.execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["data.qi"]})");
}

TEST_CASE("ArrayField: single-value assignment replaces") {
    note::ArrayField<note::string_view, 8> arr;
    arr = "first";
    REQUIRE(arr.size() == 1);
    arr = "second";
    REQUIRE(arr.size() == 1);
    REQUIRE(arr[0] == "second");
}

// ---------------------------------------------------------------------------
// Alias calling patterns
// ---------------------------------------------------------------------------

TEST_CASE("Alias: positional single arg") {
    TestHarness h;
    note::Api api(h.nc);
    api.file.remove("data.db").execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["data.db"]})");
}

TEST_CASE("Alias: args struct with array field") {
    TestHarness h;
    note::Api api(h.nc);
    api.file.remove({{"a.db", "b.db"}}).execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["a.db","b.db"]})");
}

TEST_CASE("Alias: no-arg builder") {
    TestHarness h;
    note::Api api(h.nc);
    auto req = api.file.remove();
    req.files = {"x.db", "y.db"};
    req.execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["x.db","y.db"]})");
}

TEST_CASE("Alias: multi-param positional (note.delete)") {
    TestHarness h;
    note::Api api(h.nc);
    api.note.remove("data.db", "my-note").execute();
    REQUIRE(h.last_request == R"({"req":"note.delete","file":"data.db","note":"my-note"})");
}

#if __cplusplus >= 202002L
TEST_CASE("Alias: designated init with array field") {
    TestHarness h;
    note::Api api(h.nc);
    api.file.remove({.files = {"a.db", "b.db"}}).execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["a.db","b.db"]})");
}

TEST_CASE("Alias: designated init with single file") {
    TestHarness h;
    note::Api api(h.nc);
    api.file.remove({.files = {"data.db"}}).execute();
    REQUIRE(h.last_request == R"({"req":"file.delete","files":["data.db"]})");
}
#endif

// ---------------------------------------------------------------------------
// README Developer Experience section — compilable verification
// ---------------------------------------------------------------------------

TEST_CASE("DX: intent-revealing aliases produce correct wire format") {
    TestHarness h;
    note::Api api(h.nc);

    // read by ID
    api.note.read("data.db").noteId("my-note").execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.get","file":"data.db","note":"my-note"})");

    // pop from queue
    api.note.pop("requests.qi").execute();
    REQUIRE(h.last_request ==
        R"({"req":"note.get","delete":true,"file":"requests.qi"})");

    // binary status and clear
    api.binary.status().execute();
    REQUIRE(h.last_request == R"({"req":"card.binary"})");

    api.binary.clear().execute();
    REQUIRE(h.last_request == R"({"req":"card.binary","delete":true})");
}

TEST_CASE("DX: direct assignment from application config") {
    TestHarness h;
    note::Api api(h.nc);

    // Simulate application config
    struct { const char* product_uid; const char* sync_mode; int sync_interval; }
        app_config{"com.example.app", "periodic", 60};

    auto req = api.hub.set();
    req.product  = app_config.product_uid;
    req.mode     = app_config.sync_mode;
    req.outbound = app_config.sync_interval;
    req.execute();

    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"periodic","outbound":60,"product":"com.example.app"})");
}

TEST_CASE("DX: conditional fields — unset fields omitted") {
    TestHarness h;
    note::Api api(h.nc);

    // Simulate application config
    struct { const char* product_uid; const char* sync_mode; }
        app_config{"com.example.app", "continuous"};

    auto req = api.hub.set();
    req.product = app_config.product_uid;
    req.mode    = app_config.sync_mode;
    if (app_config.sync_mode == note::string_view("continuous")) {
        req.sync = true;  // only sent in continuous mode
    }
    req.execute();

    // sync:true is present because mode is "continuous"
    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"continuous","product":"com.example.app","sync":true})");
}

TEST_CASE("DX: conditional fields — sync omitted when not continuous") {
    TestHarness h;
    note::Api api(h.nc);

    struct { const char* product_uid; const char* sync_mode; }
        app_config{"com.example.app", "periodic"};

    auto req = api.hub.set();
    req.product = app_config.product_uid;
    req.mode    = app_config.sync_mode;
    if (app_config.sync_mode == note::string_view("continuous")) {
        req.sync = true;
    }
    req.execute();

    // sync field is absent because mode is "periodic"
    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"periodic","product":"com.example.app"})");
}

// ---------------------------------------------------------------------------
// Transparent compile-time validation and named constants (C++20)
// ---------------------------------------------------------------------------

TEST_CASE("DX: string literal assignment is validated at compile time") {
    TestHarness h;
    note::Api api(h.nc);

    // Valid literals — compile and produce correct wire format
    auto req = api.hub.set();
    req.mode = "periodic";
    req.execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","mode":"periodic"})");

    // Invalid literals would fail at compile time:
    //   req.mode = "perioidc";  // error: hub.set: invalid value for 'mode'
}

TEST_CASE("DX: runtime string_view bypasses validation") {
    TestHarness h;
    note::Api api(h.nc);

    // Runtime values are not validated (may come from config, user input, etc.)
    note::string_view runtime_mode = "periodic";
    auto req = api.hub.set();
    req.mode = runtime_mode;
    req.execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","mode":"periodic"})");
}

TEST_CASE("DX: named constants on enum fields") {
    TestHarness h;
    note::Api api(h.nc);

    // Constants are discoverable via IDE autocomplete on the field type
    using mode = note::api::HubSet::mode_t;

    auto req = api.hub.set();
    req.mode = mode::periodic;
    req.execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","mode":"periodic"})");

    req.mode = mode::continuous;
    req.execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","mode":"continuous"})");
}

#if __cplusplus >= 202002L
TEST_CASE("DX: designated initializer with validated field") {
    TestHarness h;

    // Designated init — literal validated at compile time
    note::api::HubSet req{.mode = "periodic"};
    req.nc_ = &h.nc;
    req.execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","mode":"periodic"})");

    // Invalid would fail at compile time:
    //   note::api::HubSet bad{.mode = "perioidc"};  // compile error
}
#endif

TEST_CASE("DX: fluent setter validates literals") {
    TestHarness h;
    note::Api api(h.nc);

    // Fluent with literal — validated
    api.hub.set().mode("periodic").execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","mode":"periodic"})");

    // Fluent with runtime value — no validation
    note::string_view m = "continuous";
    api.hub.set().mode(m).execute();
    REQUIRE(h.last_request == R"({"req":"hub.set","mode":"continuous"})");
}

// ---------------------------------------------------------------------------
// ErrorInfo construction (must work even when inheriting Printable on Arduino)
// ---------------------------------------------------------------------------

TEST_CASE("ErrorInfo: brace construction") {
    // These mirror the construction patterns used throughout note-cpp.
    // If ErrorInfo inherits from a non-aggregate base (e.g. Printable
    // on Arduino), these must still compile.
    note::ErrorInfo e1{note::Error::SendFailed, note::Cause::Timeout, "timeout"};
    REQUIRE(e1.code == note::Error::SendFailed);
    REQUIRE(e1.cause == note::Cause::Timeout);
    REQUIRE(e1.message == "timeout");

    note::ErrorInfo e2{note::Error::Notecard, "device error"};
    REQUIRE(e2.code == note::Error::Notecard);
    REQUIRE(e2.cause == note::Cause::Unspecified);
    REQUIRE(e2.message == "device error");

    note::ErrorInfo e3{};
    (void)e3;
}

TEST_CASE("ResponseField<bool>: implicit conversion") {
    note::ResponseField<bool> f;
    f = true;
    // Implicit conversion to bool — no ambiguity (no optional base)
    bool v = f;
    REQUIRE(v == true);
    if (f) { /* works as a condition */ }
    REQUIRE(f.value() == true);
}

TEST_CASE("ResponseField<string_view>: forwarded methods") {
    note::ResponseField<note::string_view> f;
    f = "hello";
    REQUIRE(f.size() == 5);
    REQUIRE(f.data() != nullptr);
    REQUIRE(!f.empty());
    REQUIRE(f.find("ell") != note::string_view::npos);
    // Implicit conversion
    note::string_view sv = f;
    REQUIRE(sv == "hello");
}

TEST_CASE("ResponseField: default-initialized when absent") {
    note::ResponseField<bool> b;
    REQUIRE(b.value() == false);
    note::ResponseField<int32_t> i;
    REQUIRE(i.value() == 0);
    note::ResponseField<note::string_view> s;
    REQUIRE(s.empty());
}

TEST_CASE("ErrorInfo: used in Result/ApiResult") {
    // Verify ErrorInfo works in the error paths used by Notecard::execute
    note::Result<int> r1 = note::Unexpected(
        note::ErrorInfo{note::Error::SendFailed, note::Cause::Timeout, "timeout"});
    REQUIRE(!r1);
    REQUIRE(r1.error().code == note::Error::SendFailed);
    REQUIRE(r1.error().cause == note::Cause::Timeout);

    note::ErrorInfo ei{note::Error::Notecard, "device error"};
    note::ApiResult<void> r2(ei);
    REQUIRE(!r2);
    REQUIRE(r2.error().message == "device error");
}

// ---------------------------------------------------------------------------
// NotecardApi — convenience wrapper
// ---------------------------------------------------------------------------

TEST_CASE("NotecardApi: default constructor + begin()") {
    note::NotecardApi nc;

    // Before begin(), requests return NotReady error
    auto r = nc.hub.set().product("test").execute();
    REQUIRE_FALSE(r);
    REQUIRE(r.error().code == note::Error::NotReady);

    // After begin(), requests work
    std::string last_request;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            last_request = std::string(req);
            return note::string_view("{}");
        });
    nc.begin(transport);
    nc.hub.set().product("test").execute();
    REQUIRE(last_request.find("hub.set") != std::string::npos);
}

TEST_CASE("NotecardApi: construct with transport") {
    std::string last_request;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            last_request = std::string(req);
            return note::string_view("{}");
        });
    note::NotecardApi nc(transport);

    // Use Api surface directly on nc — no separate Api object needed
    nc.hub.set().product("com.example.app").mode("periodic").execute();
    REQUIRE(last_request ==
        R"({"req":"hub.set","mode":"periodic","product":"com.example.app"})");

    nc.card.version().execute();
    REQUIRE(last_request == R"({"req":"card.version"})");
}

TEST_CASE("NotecardApi: notecard() accessor") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view("{}");
        });
    note::NotecardApi nc(backend, transport);

    // Can access underlying Notecard for transport-level operations
    auto& notecard = nc.notecard();
    (void)notecard;
}
