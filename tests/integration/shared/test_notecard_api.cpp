/// @file test_notecard_api.cpp
/// Transport-agnostic integration tests for the Notecard API.
///
/// Shared across all environments (serial, I2C, softcard). Each environment
/// symlinks this file into its test/ directory and provides a global Api
/// instance via notecard_api_fixture.hpp.
///
/// The environment's main.cpp creates the transport-specific Notecard and Api,
/// storing pointers in the globals declared in notecard_api_fixture.hpp.

#include "notecard_api_fixture.hpp"

#include <doctest.h>
#include <note/notecard.hpp>
#include <note/error.hpp>
#include <note/api.hpp>
#include <note/body.hpp>
#include <note/units.hpp>

// ─── Shared types ───────────────────────────────────────────────────────────

namespace {
struct SensorData {
    float temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};
} // namespace

// ─── Basic request/response ─────────────────────────────────────────────────

TEST_CASE("card.version returns valid device info") {
    auto& nc = notecard_api();
    auto rsp = nc.card.version().execute();
    if (!rsp) { INFO(note::to_string(rsp.error())); }
    REQUIRE(rsp);
    CHECK(!note::string_view(rsp.device).empty());
    CHECK(!note::string_view(rsp.version).empty());
    MESSAGE("device: ", rsp.device);
    MESSAGE("version: ", rsp.version);
}

TEST_CASE("card.status returns operational state") {
    auto& nc = notecard_api();
    auto rsp = nc.card.status().execute();
    if (!rsp) { INFO(note::to_string(rsp.error())); }
    REQUIRE(rsp);
    CHECK(!note::string_view(rsp.status).empty());
    MESSAGE("status: ", rsp.status);
    MESSAGE("storage: ", rsp.storage, "%");
}

// ─── Configuration round-trip ───────────────────────────────────────────────

TEST_CASE("hub.set + hub.get round-trip") {
    auto& nc = notecard_api();
    auto set_rsp = nc.hub.set()
        .product("com.example.integration-test")
        .execute();
    if (!set_rsp) { INFO(note::to_string(set_rsp.error())); }
    REQUIRE(set_rsp);

    auto get_rsp = nc.hub.get().execute();
    if (!get_rsp) { INFO(note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);
    CHECK(note::string_view(get_rsp.product) == "com.example.integration-test");
}

// ─── Note lifecycle ─────────────────────────────────────────────────────────

TEST_CASE("note.add sends a note") {
    auto& nc = notecard_api();
    auto rsp = nc.note.add()
        .file("integration-test.qo")
        .execute();
    if (!rsp) { INFO(note::to_string(rsp.error())); }
    REQUIRE(rsp);
}

TEST_CASE("note.update + note.get body round-trip") {
    auto& nc = notecard_api();
    const char* file = "integration-body.db";
    const char* noteId = "test-sensor";

    SensorData sent{.temperature = 23.5f, .humidity = 65};
    auto update_rsp = nc.note.update(file, noteId)
        .body(sent)
        .execute();
    if (!update_rsp) { MESSAGE("update error: ", note::to_string(update_rsp.error())); }
    REQUIRE(update_rsp);

    auto get_rsp = nc.note.read(file)
        .noteId(noteId)
        .execute();
    if (!get_rsp) { MESSAGE("get error: ", note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);

    SensorData received = get_rsp.bodyAs<SensorData>();
    CHECK(received.temperature == doctest::Approx(sent.temperature));
    CHECK(received.humidity == sent.humidity);
}

TEST_CASE("note.changes tracks additions") {
    auto& nc = notecard_api();
    const char* file = "integration-changes.db";
    const char* note_id = "test-change";
    const char* tracker = "integration-test";

    nc.file.remove(file).execute();

    auto reset_rsp = nc.note.changes().peek()
        .file(file)
        .tracker(tracker)
        .resetTracker()
        .execute();
    if (!reset_rsp) { MESSAGE("reset error: ", note::to_string(reset_rsp.error())); }

    auto add_rsp = nc.note.add().file(file).noteId(note_id).execute();
    if (!add_rsp) { MESSAGE("add error: ", note::to_string(add_rsp.error())); }
    REQUIRE(add_rsp);

    auto rsp = nc.note.changes().peek()
        .file(file)
        .tracker(tracker)
        .execute();
    if (!rsp) { MESSAGE("changes error: ", note::to_string(rsp.error())); }
    REQUIRE(rsp);
    CHECK(rsp.changes > 0);
    MESSAGE("changes: ", rsp.changes, " total: ", rsp.total);

    nc.note.remove(file, note_id).execute();
}

// ─── Environment variables ──────────────────────────────────────────────────

TEST_CASE("env.default set + get round-trip") {
    auto& nc = notecard_api();

    auto set_rsp = nc.env.setDefault("_integration_test_var", "hello-from-note-cpp")
        .execute();
    if (!set_rsp) { INFO(note::to_string(set_rsp.error())); }
    REQUIRE(set_rsp);

    auto get_rsp = nc.env.get().name("_integration_test_var").execute();
    if (!get_rsp) { INFO(note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);
    CHECK(note::string_view(get_rsp.text) == "hello-from-note-cpp");

    nc.env.clearDefault("_integration_test_var").execute();
}

// ─── Error handling ─────────────────────────────────────────────────────────

namespace { struct MaxTestPayload { int32_t a; NOTE_FIELDS(a) }; }

TEST_CASE("note.add max limit produces Notecard error") {
    auto& nc = notecard_api();
    const char* file = "integration-err.qo";

    nc.file.remove(file).execute();

    auto r1 = nc.note.add().file(file).body(MaxTestPayload{1}).max(1).execute();
    if (!r1) { MESSAGE("add error: ", note::to_string(r1.error())); }
    REQUIRE(r1);

    auto r2 = nc.note.add().file(file).body(MaxTestPayload{2}).max(1).execute();
    REQUIRE_FALSE(r2);
    REQUIRE(r2.error().code == note::Error::Notecard);
    MESSAGE("Notecard error: ", r2.error().message);

    nc.file.remove(file).execute();
}

TEST_CASE("bad request returns Notecard error") {
    auto& nc = notecard_api();
    auto rsp = nc.note.pop("nonexistent-file.qi").execute();
    CHECK(!rsp);
    if (!rsp) {
        CHECK(rsp.error().code == note::Error::Notecard);
        MESSAGE("error: ", note::to_string(rsp.error()));
    }
}

// ─── Version-gated API fields ───────────────────────────────────────────────

TEST_SUITE("fw>=3.2.1") {
TEST_CASE("card.attn verify field (3.2.1+)") {
    auto& nc = notecard_api();
    auto rsp = nc.card.attn().request().verify(true).execute();
    MESSAGE("card.attn verify: ", rsp ? "ok" : note::to_string(rsp.error()));
}
} // fw>=3.2.1

TEST_SUITE("fw>=3.4.1") {
TEST_CASE("env.get with names filter (3.4.1+)") {
    auto& nc = notecard_api();
    auto req = nc.env.get();
    req.names = {"_*"};
    auto rsp = req.execute();
    if (!rsp) { MESSAGE("env.get names: ", note::to_string(rsp.error())); }
    CHECK(rsp);
}
} // fw>=3.4.1

TEST_SUITE("fw>=5.1.1") {
TEST_CASE("note.add full field (5.1.1+)") {
    auto& nc = notecard_api();
    auto rsp = nc.note.add().file("_integration_fw_gate.qo").full(true).execute();
    if (!rsp) { MESSAGE("note.add full: ", note::to_string(rsp.error())); }
    CHECK(rsp);
    nc.file.remove("_integration_fw_gate.qo").execute();
}
} // fw>=5.1.1

TEST_SUITE("fw>=5.3.1") {
TEST_CASE("card.transport seconds field (5.3.1+)") {
    auto& nc = notecard_api();
    auto rsp = nc.card.transport().seconds(note::Seconds{3600}).execute();
    if (!rsp) { MESSAGE("card.transport seconds: ", note::to_string(rsp.error())); }
    CHECK(rsp);
}
} // fw>=5.3.1

TEST_SUITE("fw>=8.2.1") {
TEST_CASE("note.add max field (8.2.1+)") {
    auto& nc = notecard_api();
    auto rsp = nc.note.add().file("_integration_fw_gate.qo").max(10).execute();
    if (!rsp) { MESSAGE("note.add max: ", note::to_string(rsp.error())); }
    CHECK(rsp);
    nc.file.remove("_integration_fw_gate.qo").execute();
}
} // fw>=8.2.1

TEST_SUITE("fw>=9.1.1") {
TEST_CASE("note.add limit field (9.1.1+)") {
    auto& nc = notecard_api();
    auto rsp = nc.note.add().file("_integration_fw_gate.qo").limit(100).execute();
    if (!rsp) { MESSAGE("note.add limit: ", note::to_string(rsp.error())); }
    CHECK(rsp);
    nc.file.remove("_integration_fw_gate.qo").execute();
}
} // fw>=9.1.1
