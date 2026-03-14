// Tests for Attention — enable/disable mode keywords, arm,
// triggered, query, and StateStore integration.

#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/app/channel.hpp>
#include <note/app/attention_manager.hpp>
#include <note/app/state_store.hpp>

using Store = note::app::StaticStateStore<note::app::AttentionState>;

namespace {

struct TestFixture {
    note::test::TestJsonBackend backend;
    std::vector<std::string> captured;
    note::Notecard nc;
    note::app::DirectChannel ch;
    Store store;

    TestFixture()
        : nc(backend,
            [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                captured.emplace_back(req);
                return std::string("{}");
            })
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
    REQUIRE(f.captured[0].find("card.attn") != std::string::npos);
    REQUIRE(f.captured[0].find("files") != std::string::npos);
}

TEST_CASE("Attention::arm() sends card.attn with timeout") {
    TestFixture f;
    note::app::Attention<note::app::DirectChannel, Store> attn(f.ch, f.store);

    attn.enable("sleep");
    auto r = attn.arm(note::Seconds{300});
    REQUIRE(r.has_value());
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
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::make_error(note::Error::SendFailed, "write failed");
        });
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Attention<note::app::DirectChannel, Store> attn(ch, store);

    auto r = attn.arm();
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}
