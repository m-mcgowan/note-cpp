// Tests for Connection — configure(), status(), is_connected(),
// error propagation, and StateStore integration.

#include <doctest.h>
#include <string>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/app/channel.hpp>
#include <note/app/connection_manager.hpp>
#include <note/app/state_store.hpp>

using Store = note::app::StaticStateStore<note::app::ConnectionState>;

namespace {

// Helper: create a Notecard + DirectChannel that captures requests.
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
// configure() sends hub.set
// ---------------------------------------------------------------------------

TEST_CASE("Connection::configure() sends hub.set") {
    TestFixture f;
    note::app::Connection<note::app::DirectChannel, Store> conn(f.ch, f.store);

    note::api::HubSet config;
    config.product("com.example.app");
    config.mode("periodic");
    auto r = conn.configure(config);
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("hub.set") != std::string::npos);
    REQUIRE(f.captured[0].find("com.example.app") != std::string::npos);
}

// ---------------------------------------------------------------------------
// configure() propagates transport errors
// ---------------------------------------------------------------------------

TEST_CASE("Connection::configure() propagates transport errors") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::make_error(note::Error::SendFailed, "write failed");
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Connection<note::app::DirectChannel, Store> conn(ch, store);

    auto r = conn.configure(note::api::HubSet{});
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

// ---------------------------------------------------------------------------
// status() queries hub.status and updates store
// ---------------------------------------------------------------------------

TEST_CASE("Connection::status() queries hub.status") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured = std::string(req);
            return note::string_view("{}");
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Connection<note::app::DirectChannel, Store> conn(ch, store);

    auto r = conn.status();
    REQUIRE(r);
    REQUIRE(captured.find("hub.status") != std::string::npos);

    auto s = store.get<note::app::ConnectionState>();
    REQUIRE(s.has_value());
}

// ---------------------------------------------------------------------------
// is_connected() reads from store when available
// ---------------------------------------------------------------------------

TEST_CASE("Connection::is_connected() reads from store") {
    TestFixture f;
    note::app::Connection<note::app::DirectChannel, Store> conn(f.ch, f.store);

    // Pre-populate store
    f.store.set(note::app::ConnectionState{.connected = true, .status = "connected"});

    auto r = conn.is_connected();
    REQUIRE(r.has_value());
    REQUIRE(*r == true);
    // Should not have sent any request
    REQUIRE(f.captured.empty());
}

// ---------------------------------------------------------------------------
// is_connected() queries when store is empty
// ---------------------------------------------------------------------------

TEST_CASE("Connection::is_connected() queries when store is empty") {
    TestFixture f;
    note::app::Connection<note::app::DirectChannel, Store> conn(f.ch, f.store);

    auto r = conn.is_connected();
    REQUIRE(r.has_value());
    // Should have queried hub.status
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("hub.status") != std::string::npos);
}
