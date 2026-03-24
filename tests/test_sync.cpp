// Tests for Sync — sync(), sync_outbound(), sync_inbound(),
// NTN directional split, status(), wait_for_sync(), error propagation.

#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/app/channel.hpp>
#include <note/app/sync_manager.hpp>
#include <note/app/state_store.hpp>

using Store = note::app::StaticStateStore<note::app::SyncState>;

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
        , nc(backend, transport)
        , ch(nc) {}
};

} // namespace

// ---------------------------------------------------------------------------
// sync() in normal mode sends a single hub.sync
// ---------------------------------------------------------------------------

TEST_CASE("Sync::sync() sends hub.sync in normal mode") {
    TestFixture f;
    note::app::Sync<note::app::DirectChannel, Store> sync(f.ch, f.store);

    auto r = sync.sync();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("hub.sync") != std::string::npos);
}

// ---------------------------------------------------------------------------
// sync() in NTN mode sends two requests (out + in)
// ---------------------------------------------------------------------------

TEST_CASE("Sync::sync() sends two requests in NTN mode") {
    TestFixture f;
    note::app::Sync<note::app::DirectChannel, Store> sync(f.ch, f.store);
    sync.set_ntn(true);

    auto r = sync.sync();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 2);
    REQUIRE(f.captured[0].find("\"out\":true") != std::string::npos);
    REQUIRE(f.captured[1].find("\"in\":true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// sync_outbound() in NTN mode sends out:true
// ---------------------------------------------------------------------------

TEST_CASE("Sync::sync_outbound() sends out:true in NTN mode") {
    TestFixture f;
    note::app::Sync<note::app::DirectChannel, Store> sync(f.ch, f.store);
    sync.set_ntn(true);

    auto r = sync.sync_outbound();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("\"out\":true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// sync_outbound() in normal mode sends plain hub.sync
// ---------------------------------------------------------------------------

TEST_CASE("Sync::sync_outbound() sends plain hub.sync in normal mode") {
    TestFixture f;
    note::app::Sync<note::app::DirectChannel, Store> sync(f.ch, f.store);

    auto r = sync.sync_outbound();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("hub.sync") != std::string::npos);
    REQUIRE(f.captured[0].find("\"out\"") == std::string::npos);
}

// ---------------------------------------------------------------------------
// sync_inbound() in NTN mode sends in:true
// ---------------------------------------------------------------------------

TEST_CASE("Sync::sync_inbound() sends in:true in NTN mode") {
    TestFixture f;
    note::app::Sync<note::app::DirectChannel, Store> sync(f.ch, f.store);
    sync.set_ntn(true);

    auto r = sync.sync_inbound();
    REQUIRE(r.has_value());
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("\"in\":true") != std::string::npos);
}

// ---------------------------------------------------------------------------
// status() queries hub.sync.status and updates store
// ---------------------------------------------------------------------------

TEST_CASE("Sync::status() queries hub.sync.status") {
    TestFixture f;
    note::app::Sync<note::app::DirectChannel, Store> sync(f.ch, f.store);

    auto r = sync.status();
    REQUIRE(r);
    REQUIRE(f.captured.size() == 1);
    REQUIRE(f.captured[0].find("hub.sync.status") != std::string::npos);

    auto s = f.store.get<note::app::SyncState>();
    REQUIRE(s.has_value());
}

// ---------------------------------------------------------------------------
// wait_for_sync() returns on sync-end
// ---------------------------------------------------------------------------

TEST_CASE("Sync::wait_for_sync() returns immediately when already complete") {
    // Provide a response with {sync-end} status
    auto reader = std::make_unique<note::test::PopulatedJsonReader>();
    reader->set("status", std::string("{sync-end}"));
    auto reader_ptr = reader.get();

    note::test::TestJsonBackend backend;
    // Override parse_response to return our populated reader
    struct StatusBackend : note::JsonBackend {
        note::test::PopulatedJsonReader* reader_;
        bool used_ = false;
        StatusBackend(note::test::PopulatedJsonReader* r) : reader_(r) {}
        std::unique_ptr<note::JsonBuilder> create_builder() override {
            return std::make_unique<note::test::TestJsonBuilder>();
        }
        std::unique_ptr<note::JsonReader> parse_response(note::string_view) override {
            if (!used_) {
                used_ = true;
                auto clone = std::make_unique<note::test::PopulatedJsonReader>();
                clone->set("status", std::string("{sync-end}"));
                return clone;
            }
            return std::make_unique<note::test::TestJsonReader>();
        }
    } status_backend(reader_ptr);

    note::CallbackTransport status_transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view("{}");
        });
    note::Notecard nc(status_backend, status_transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Sync<note::app::DirectChannel, Store> sync(ch, store);

    int polls = 0;
    auto r = sync.wait_for_sync(10, [&]{ ++polls; });
    REQUIRE(r.has_value());
    REQUIRE(polls == 0);  // completed on first poll
}

// ---------------------------------------------------------------------------
// wait_for_sync() times out
// ---------------------------------------------------------------------------

TEST_CASE("Sync::wait_for_sync() times out after max_polls") {
    TestFixture f;
    note::app::Sync<note::app::DirectChannel, Store> sync(f.ch, f.store);

    int polls = 0;
    auto r = sync.wait_for_sync(3, [&]{ ++polls; });
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::ResponseLost);
    REQUIRE(r.error().cause == note::Cause::Timeout);
    REQUIRE(polls == 2);  // max_polls - 1 pauses
}

// ---------------------------------------------------------------------------
// Error propagation
// ---------------------------------------------------------------------------

TEST_CASE("Sync::sync() propagates transport errors") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::make_error(note::Error::SendFailed, "write failed");
        });
    note::Notecard nc(backend, transport);
    note::app::DirectChannel ch(nc);
    Store store;
    note::app::Sync<note::app::DirectChannel, Store> sync(ch, store);

    auto r = sync.sync();
    REQUIRE(!r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}
