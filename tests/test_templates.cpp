// Tests for Templates — declare, register_all, NTN validation,
// idempotent re-register, error propagation.

#include "catch.hpp"
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/app/channel.hpp>
#include <note/app/template_manager.hpp>
#include <note/app/state_store.hpp>
#include <note/body.hpp>

using Store = note::app::NullStateStore;

namespace {

struct TestFixture {
    note::test::TestJsonBackend backend;
    std::vector<std::string> captured;
    note::CallbackTransport transport;
    note::Notecard nc;
    note::app::DirectChannel ch;

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
// register_all() sends note.template for each declared template
// ---------------------------------------------------------------------------

TEST_CASE("Templates::register_all() sends note.template for each entry") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);

    tmpl.declare("readings.qo", note::BodyValue{});
    tmpl.declare("config.db", note::BodyValue{});

    auto r = tmpl.register_all();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 2);
    REQUIRE(f.captured[0].find("note.template") != std::string::npos);
    REQUIRE(f.captured[0].find("readings.qo") != std::string::npos);
    REQUIRE(f.captured[1].find("config.db") != std::string::npos);
}

// ---------------------------------------------------------------------------
// register_all() is idempotent — doesn't re-register
// ---------------------------------------------------------------------------

TEST_CASE("Templates::register_all() skips already registered") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);

    tmpl.declare("readings.qo", note::BodyValue{});

    REQUIRE(tmpl.register_all().has_value());
    REQUIRE(f.captured.size() == 1);

    // Second call should not send anything
    REQUIRE(tmpl.register_all().has_value());
    REQUIRE(f.captured.size() == 1);
}

// ---------------------------------------------------------------------------
// is_registered() tracks registration state
// ---------------------------------------------------------------------------

TEST_CASE("Templates::is_registered() tracks state") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);

    tmpl.declare("readings.qo", note::BodyValue{});
    REQUIRE(!tmpl.is_registered("readings.qo"));

    tmpl.register_all();
    REQUIRE(tmpl.is_registered("readings.qo"));
    REQUIRE(!tmpl.is_registered("unknown.qo"));
}

// ---------------------------------------------------------------------------
// reset() clears registration state
// ---------------------------------------------------------------------------

TEST_CASE("Templates::reset() clears registration") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);

    tmpl.declare("readings.qo", note::BodyValue{});
    tmpl.register_all();
    REQUIRE(tmpl.is_registered("readings.qo"));

    tmpl.reset();
    REQUIRE(!tmpl.is_registered("readings.qo"));

    // Re-register sends again
    tmpl.register_all();
    REQUIRE(f.captured.size() == 2);
}

// ---------------------------------------------------------------------------
// NTN mode validates port
// ---------------------------------------------------------------------------

TEST_CASE("Templates NTN mode rejects missing port") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);
    tmpl.set_ntn(true);

    tmpl.declare("readings.qo", note::BodyValue{}, 0, true);  // port=0, compact=true

    auto r = tmpl.register_all();
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::InvalidArg);
}

// ---------------------------------------------------------------------------
// NTN mode validates compact format
// ---------------------------------------------------------------------------

TEST_CASE("Templates NTN mode rejects non-compact") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);
    tmpl.set_ntn(true);

    tmpl.declare("readings.qo", note::BodyValue{}, 55, false);  // port=55, compact=false

    auto r = tmpl.register_all();
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::InvalidArg);
}

// ---------------------------------------------------------------------------
// NTN mode accepts valid template
// ---------------------------------------------------------------------------

TEST_CASE("Templates NTN mode accepts valid template") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);
    tmpl.set_ntn(true);

    tmpl.declare("readings.qo", note::BodyValue{}, 55, true);

    auto r = tmpl.register_all();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("\"port\":55") != std::string::npos);
    REQUIRE(f.captured[0].find("\"format\":\"compact\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Port is included even without NTN mode
// ---------------------------------------------------------------------------

TEST_CASE("Templates includes port when set") {
    TestFixture f;
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(f.ch, store);

    tmpl.declare("readings.qo", note::BodyValue{}, 42);

    auto r = tmpl.register_all();
    REQUIRE(r.has_value());
    REQUIRE(f.captured[0].find("\"port\":42") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Error propagation
// ---------------------------------------------------------------------------

TEST_CASE("Templates::register_all() propagates transport errors") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::make_error(note::Error::SendFailed, "write failed");
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Templates<note::app::DirectChannel, Store> tmpl(ch, store);

    tmpl.declare("readings.qo", note::BodyValue{});
    auto r = tmpl.register_all();
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}
