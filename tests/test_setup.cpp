// Tests for Setup — full sequence, NTN mode, fixed location,
// error propagation at each step, sub-manager access.

#include "catch.hpp"
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/app/channel.hpp>
#include <note/app/setup.hpp>
#include <note/app/state_store.hpp>

using Store = note::app::NullStateStore;

namespace {

struct TestFixture {
    note::test::TestJsonBackend backend;
    std::vector<std::string> captured;
    note::CallbackTransport transport;
    note::Notecard nc;
    note::app::DirectChannel ch;
    Store store;

    TestFixture()
        : transport(
            [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                captured.emplace_back(req);
                return note::string_view("{}");
            })
        , nc(note::test::make_test_notecard(backend, transport))
        , ch(nc) {}
};

} // namespace

// ---------------------------------------------------------------------------
// Basic setup sends hub.set + hub.sync
// ---------------------------------------------------------------------------

TEST_CASE("Setup::run() sends hub.set then hub.sync") {
    TestFixture f;
    note::app::Setup<note::app::DirectChannel, Store> setup(f.ch, f.store);

    setup.product("com.example.app").mode("periodic");
    auto r = setup.run();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 2);  // hub.set + hub.sync
    REQUIRE(f.captured[0].find("hub.set") != std::string::npos);
    REQUIRE(f.captured[0].find("com.example.app") != std::string::npos);
    REQUIRE(f.captured[0].find("periodic") != std::string::npos);
    REQUIRE(f.captured[1].find("hub.sync") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Setup with templates
// ---------------------------------------------------------------------------

TEST_CASE("Setup::run() registers templates") {
    TestFixture f;
    note::app::Setup<note::app::DirectChannel, Store> setup(f.ch, f.store);

    setup.product("com.example.app")
         .mode("periodic")
         .template_("readings.qo", note::BodyValue{});

    auto r = setup.run();
    REQUIRE(r.has_value());
    // hub.set + note.template + hub.sync
    REQUIRE(f.captured.size() == 3);
    REQUIRE(f.captured[1].find("note.template") != std::string::npos);
    REQUIRE(f.captured[1].find("readings.qo") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Setup with fixed location
// ---------------------------------------------------------------------------

TEST_CASE("Setup::run() sends card.location.mode for fixed location") {
    TestFixture f;
    note::app::Setup<note::app::DirectChannel, Store> setup(f.ch, f.store);

    setup.product("com.example.app")
         .fixed_location(42.565, -70.783);

    auto r = setup.run();
    REQUIRE(r.has_value());
    // hub.set + hub.sync + card.location.mode
    REQUIRE(f.captured.size() == 3);
    REQUIRE(f.captured[2].find("card.location.mode") != std::string::npos);
    REQUIRE(f.captured[2].find("\"mode\":\"fixed\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Setup with outbound/inbound
// ---------------------------------------------------------------------------

TEST_CASE("Setup::run() includes outbound and inbound") {
    TestFixture f;
    note::app::Setup<note::app::DirectChannel, Store> setup(f.ch, f.store);

    using namespace note::literals;
    setup.outbound(15_mins).inbound(120_minutes);

    auto r = setup.run();
    REQUIRE(r.has_value());
    REQUIRE(f.captured[0].find("\"outbound\":15") != std::string::npos);
    REQUIRE(f.captured[0].find("\"inbound\":120") != std::string::npos);
}

// ---------------------------------------------------------------------------
// NTN mode enables directional sync
// ---------------------------------------------------------------------------

TEST_CASE("Setup NTN mode uses directional sync") {
    TestFixture f;
    note::app::Setup<note::app::DirectChannel, Store> setup(f.ch, f.store);

    setup.product("com.example.app").ntn();

    auto r = setup.run();
    REQUIRE(r.has_value());
    // hub.set + hub.sync(out) + hub.sync(in)
    REQUIRE(f.captured.size() == 3);
    REQUIRE(f.captured[1].find("\"out\":true") != std::string::npos);
    REQUIRE(f.captured[2].find("\"in\":true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// NTN mode auto-sets compact on templates
// ---------------------------------------------------------------------------

TEST_CASE("Setup NTN mode auto-sets compact on templates") {
    TestFixture f;
    note::app::Setup<note::app::DirectChannel, Store> setup(f.ch, f.store);

    setup.product("com.example.app")
         .ntn()
         .template_("readings.qo", note::BodyValue{}, 55);

    auto r = setup.run();
    REQUIRE(r.has_value());
    // Find the note.template request
    bool found = false;
    for (const auto& req : f.captured) {
        if (req.find("note.template") != std::string::npos) {
            REQUIRE(req.find("\"format\":\"compact\"") != std::string::npos);
            REQUIRE(req.find("\"port\":55") != std::string::npos);
            found = true;
        }
    }
    REQUIRE(found);
}

// ---------------------------------------------------------------------------
// Error at hub.set step
// ---------------------------------------------------------------------------

TEST_CASE("Setup::run() fails at hub.set step") {
    int call_count = 0;
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [&](note::string_view, uint32_t) -> note::Result<note::string_view> {
            if (call_count++ == 0)
                return note::make_error(note::Error::SendFailed, "write failed");
            return note::string_view("{}");
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Setup<note::app::DirectChannel, Store> setup(ch, store);

    setup.product("com.example.app");
    auto r = setup.run();
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

// ---------------------------------------------------------------------------
// Error at template registration step
// ---------------------------------------------------------------------------

TEST_CASE("Setup::run() fails at template step") {
    int call_count = 0;
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [&](note::string_view, uint32_t) -> note::Result<note::string_view> {
            if (call_count++ == 1)  // second call = note.template
                return note::make_error(note::Error::SendFailed, "write failed");
            return note::string_view("{}");
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Setup<note::app::DirectChannel, Store> setup(ch, store);

    setup.product("com.example.app")
         .template_("readings.qo", note::BodyValue{});

    auto r = setup.run();
    REQUIRE(!r.has_value());
}

// ---------------------------------------------------------------------------
// Sub-manager access
// ---------------------------------------------------------------------------

TEST_CASE("Setup provides access to sub-managers") {
    TestFixture f;
    note::app::Setup<note::app::DirectChannel, Store> setup(f.ch, f.store);

    // Just verify these compile and return references
    auto& conn = setup.connection();
    auto& tmpl = setup.templates();
    auto& sync = setup.sync();
    (void)conn; (void)tmpl; (void)sync;
}
