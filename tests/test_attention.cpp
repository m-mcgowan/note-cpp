// Tests for Attention — enable/disable mode keywords, arm,
// triggered, query, and StateStore integration.

#include <doctest.h>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/api.hpp>
#include <note/app/channel.hpp>
#include <note/app/attention_manager.hpp>
#include <note/app/state_store.hpp>

using Store = note::app::StaticStateStore<note::app::AttentionState>;

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
// enable() / disable() build mode string
// ---------------------------------------------------------------------------

TEST_CASE("Attention::enable() adds keyword") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    REQUIRE(attn.mode() == "files");

    attn.enable("sleep");
    REQUIRE(attn.mode() == "files,sleep");
}

TEST_CASE("Attention::enable() is idempotent") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    attn.enable("files");
    REQUIRE(attn.mode() == "files");
}

TEST_CASE("Attention::disable() removes keyword") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    attn.enable("sleep");
    attn.enable("motion");
    attn.disable("sleep");
    REQUIRE(attn.mode() == "files,motion");
}

TEST_CASE("Attention::disable() handles single keyword") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    attn.disable("files");
    REQUIRE(attn.mode().empty());
}

TEST_CASE("Attention::disable() no-op for absent keyword") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    attn.disable("motion");
    REQUIRE(attn.mode() == "files");
}

// ---------------------------------------------------------------------------
// arm() sends card.attn with mode
// ---------------------------------------------------------------------------

TEST_CASE("Attention::arm() sends card.attn with mode") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    auto r = attn.arm();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 1);
    // Wire format must be "arm,<triggers>" — the "arm" prefix is mandatory
    REQUIRE(f.captured[0].find("\"mode\":\"arm,files\"") != std::string::npos);
}

TEST_CASE("Attention::arm() with no triggers sends mode=arm") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    auto r = attn.arm();
    REQUIRE(r.has_value());
    REQUIRE(f.captured[0].find("\"mode\":\"arm\"") != std::string::npos);
}

TEST_CASE("Attention::arm() with multiple triggers") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    attn.enable("env");
    auto r = attn.arm();
    REQUIRE(r.has_value());
    REQUIRE(f.captured[0].find("\"mode\":\"arm,files,env\"") != std::string::npos);
}

TEST_CASE("Attention::arm() sends card.attn with timeout") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("sleep");
    auto r = attn.arm(note::Seconds{300});
    REQUIRE(r.has_value());
    REQUIRE(f.captured[0].find("\"mode\":\"arm,sleep\"") != std::string::npos);
    REQUIRE(f.captured[0].find("\"seconds\":300") != std::string::npos);
}

TEST_CASE("Attention::arm() updates store") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("files");
    attn.arm();
    auto s = f.store.get<note::app::AttentionState>();
    REQUIRE(s.has_value());
    REQUIRE(s->mode == "files");
}

// ---------------------------------------------------------------------------
// triggered() checks pin state
// ---------------------------------------------------------------------------

TEST_CASE("Attention::triggered() sends card.attn") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    auto r = attn.triggered();
    REQUIRE(r.has_value());
    REQUIRE(*r == false);  // default response has set=false
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("card.attn") != std::string::npos);
}

// ---------------------------------------------------------------------------
// query() sends card.attn with verify
// ---------------------------------------------------------------------------

TEST_CASE("Attention::query() sends card.attn with verify") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    auto r = attn.query();
    REQUIRE(r);
    REQUIRE(f.captured[0].find("\"verify\":true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Error propagation
// ---------------------------------------------------------------------------

TEST_CASE("Attention::arm() propagates transport errors") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::make_error(note::Error::SendFailed, "write failed");
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Attention<note::app::DirectChannel, Store> attn(ch, store);

    auto r = attn.arm();
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

// ---------------------------------------------------------------------------
// CardAttn::Arm raw API — mode prefix in build()
// ---------------------------------------------------------------------------

TEST_CASE("CardAttn::Arm build() emits mode=arm when no mode set") {
    TestFixture f;
    note::Api api(f.nc);
    api.card.attn().arm().execute();
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("\"mode\":\"arm\"") != std::string::npos);
}

TEST_CASE("CardAttn::Arm build() prepends arm, to trigger sources (string)") {
    TestFixture f;
    note::Api api(f.nc);
    auto req = api.card.attn().arm();
    req.triggers("files,env");  // string assignment for predictable ordering
    req.execute();
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("\"mode\":\"arm,files,env\"") != std::string::npos);
}

TEST_CASE("CardAttn::Arm build() prepends arm, to trigger sources (flags)") {
    TestFixture f;
    note::Api api(f.nc);
    auto req = api.card.attn().arm();
    req.triggers = note::attn::files;  // single flag
    req.execute();
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("\"mode\":\"arm,files\"") != std::string::npos);
}
