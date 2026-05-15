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
#include <note/backends/buffer.hpp>
#include <note/debug.hpp>
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

    SensorData received{};
    auto get_rsp = nc.note.read(file)
        .noteId(noteId)
        .into(received)
        .execute();
    if (!get_rsp) { MESSAGE("get error: ", note::to_string(get_rsp.error())); }
    REQUIRE(get_rsp);

    CHECK(received.temperature == doctest::Approx(sent.temperature));
    CHECK(received.humidity == sent.humidity);
}

// `.into(T&)` is part of the high-level API contract, not a streaming-only
// feature. This test pairs the streaming round-trip above with a buffered
// equivalent that hits the same physical transport (Notecard hardware). If
// the body dispatch ever regresses on the buffered execute path, this case
// fails on real hardware before any user does.
TEST_CASE(".into() populates body via buffered Notecard on real hardware") {
    REQUIRE(g_streaming_transport != nullptr);

    // Protocol directly satisfies ITransact now — passing it
    // to the buffered Notecard ctor lights up tree-mode body() and
    // sink-mode .into() over the same physical Notecard the streaming
    // g_api is talking to.
    note::backends::StaticJsonBackend<1024, 64> backend;
    note::Notecard nc(backend, *g_streaming_transport);
    note::Api<> api(nc);

    const char* file = "integration-into-buffered.db";
    const char* noteId = "test-into";

    SensorData sent{.temperature = 24.75f, .humidity = 55};
    auto upd = api.note.update(file, noteId).body(sent).execute();
    if (!upd) { MESSAGE("update error: ", note::to_string(upd.error())); }
    REQUIRE(upd);

    SensorData received{};
    auto rd = api.note.read(file).noteId(noteId).into(received).execute();
    if (!rd) { MESSAGE("read error: ", note::to_string(rd.error())); }
    REQUIRE(rd);

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

// ─── Inter-transaction timing ───────────────────────────────────────────────
//
// note-cpp has no inter-transaction delay. Consecutive execute() calls go
// back-to-back with zero gap. note-c has implicit overhead (~100ms+ from
// malloc, J* tree ops, _TransactionStart hooks) that gives the Notecard
// breathing room between requests.
//
// Bug: after card.attn fires (env changed), an immediate env.get returns
// empty because the Notecard hasn't finished processing. A 2-second delay
// was required as a workaround.

TEST_CASE("rapid env set+get burst — inter-transaction timing") {
    auto& nc = notecard_api();
    constexpr int BURST_SIZE = 10;
    int failures = 0;

    for (int i = 0; i < BURST_SIZE; ++i) {
        char val[32];
        snprintf(val, sizeof(val), "burst-%d", i);

        auto set_rsp = nc.env.setDefault("_burst_timing", val).execute();
        INFO("set iteration ", i);
        REQUIRE(set_rsp);

        // Immediate read — no inter-transaction delay
        auto get_rsp = nc.env.get().name("_burst_timing").execute();
        INFO("get iteration ", i);
        if (!get_rsp) { MESSAGE("get error: ", note::to_string(get_rsp.error())); }
        REQUIRE(get_rsp);

        if (note::string_view(get_rsp.text) != val) {
            MESSAGE("TIMING BUG at iteration ", i, ": expected '", val,
                    "' got '", get_rsp.text.data(), "'");
            ++failures;
        }
    }

    CHECK_MESSAGE(failures == 0,
        "Inter-transaction timing: ", failures, "/", BURST_SIZE,
        " reads returned stale or empty data");

    nc.env.clearDefault("_burst_timing").execute();
}

TEST_CASE("note.add then immediate note.changes — inter-transaction timing") {
    auto& nc = notecard_api();
    const char* file = "test-timing-changes.db";
    const char* tracker = "test-timing-tracker";

    // Clean slate
    nc.file.remove(file).execute();
    auto reset = nc.note.changes().peek()
        .file(file).tracker(tracker).resetTracker().execute();
    if (!reset) { MESSAGE("reset: ", note::to_string(reset.error())); }

    // Add a note (database notefiles require a noteId) and immediately check changes
    auto add_rsp = nc.note.add().file(file).noteId("timing-test").execute();
    if (!add_rsp) { MESSAGE("add error: ", note::to_string(add_rsp.error())); }
    REQUIRE(add_rsp);

    auto changes_rsp = nc.note.changes().peek()
        .file(file).tracker(tracker).execute();
    if (!changes_rsp) { MESSAGE("changes error: ", note::to_string(changes_rsp.error())); }
    REQUIRE(changes_rsp);
    CHECK_MESSAGE(changes_rsp.changes > 0,
        "Inter-transaction timing: note.changes returned 0 immediately after note.add");

    // Cleanup
    nc.file.remove(file).execute();
}

TEST_CASE("back-to-back diverse transactions — inter-transaction timing") {
    auto& nc = notecard_api();

    // Fire 5 different request types in rapid succession.
    // If the transport has no inter-transaction gap, the Notecard may
    // not have finished post-processing one request before the next arrives.
    auto r1 = nc.card.version().execute();
    REQUIRE(r1);
    auto r2 = nc.card.status().execute();
    REQUIRE(r2);
    auto r3 = nc.hub.get().execute();
    REQUIRE(r3);
    auto r4 = nc.card.version().execute();
    REQUIRE(r4);
    auto r5 = nc.card.status().execute();
    REQUIRE(r5);

    // Verify no data corruption across the burst
    CHECK(note::string_view(r1.device) == note::string_view(r4.device));
    CHECK(note::string_view(r1.version) == note::string_view(r4.version));
    CHECK(!note::string_view(r2.status).empty());
    CHECK(!note::string_view(r5.status).empty());
}

// ─── note.template shape validation ─────────────────────────────────────────
//
// Probe whether the Notecard stores nested / array-of-struct templates in
// their stated shape or silently flattens them. Validation approach:
//
//   1. Register a template using a lambda body (independent of
//      note::template_of, so these tests compile regardless of
//      detail::notecard_supports_nested_templates_v).
//   2. Add a note whose body exercises the nested values.
//   3. Read the note back and check every nested field round-trips.
//
// If the Notecard flattens the template or prunes unknown fields, the
// round-trip fields come back zero and the CHECKs fail. That's the
// signal that detail::notecard_supports_nested_templates_v must stay
// false. If round-trip succeeds, flipping the flag is safe.

namespace {
struct TmplInnerPoint {
    double lat;
    double lon;
    NOTE_FIELDS(lat, lon)
};
struct TmplOuterWithNested {
    float temp;
    TmplInnerPoint pos;
    NOTE_FIELDS(temp, pos)
};
struct TmplArrayOfStructs {
    std::array<TmplInnerPoint, 2> waypoints;
    NOTE_FIELDS(waypoints)
};
} // namespace

TEST_CASE("note.template flat body — known-good baseline") {
    auto& nc = notecard_api();
    const char* file = "integration-tmpl-flat.qo";
    nc.file.remove(file).execute();

    auto r = nc.note.templates().define(file)
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", note::json_int_t{11});
        }))
        .execute();
    if (!r) { MESSAGE("flat template error: ", note::to_string(r.error())); }
    CHECK(r);
    nc.file.remove(file).execute();
}

TEST_CASE("note.template — does the Notecard accept NESTED templates?") {
    auto& nc = notecard_api();
    // Must be a .db (database) notefile — note.update doesn't work on .qo
    // (queue) files. The Notecard returns "note.update is not allowed in
    // queue notefiles" if you try.
    const char* file = "integration-tmpl-nested.db";
    const char* noteId = "nested-rt";
    nc.file.remove(file).execute();

    // Step 1: register nested template via lambda.
    auto reg = nc.note.templates().define(file)
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 14.1);
            b.begin_object("pos");
                b.add("lat", 14.1);
                b.add("lon", 14.1);
            b.end_object();
        }))
        .execute();
    if (!reg) {
        MESSAGE("Notecard REJECTED nested template at registration: ",
                note::to_string(reg.error()));
        MESSAGE("→ keep detail::notecard_supports_nested_templates_v = false.");
        REQUIRE_FALSE(reg);   // fails loud if later someone expects success
        nc.file.remove(file).execute();
        return;
    }
    MESSAGE("Nested template accepted at registration — probing round-trip.");

    // Step 2: add a note with nested values.
    TmplOuterWithNested sent{.temp = 22.5f, .pos = {42.125, -71.375}};
    auto add = nc.note.update(file, noteId).body(sent).execute();
    if (!add) { MESSAGE("nested note.update error: ",
                         note::to_string(add.error())); }
    REQUIRE(add);

    // Step 3: read it back and verify every nested field survived.
    TmplOuterWithNested recv{};
    auto rd = nc.note.read(file).noteId(noteId).into(recv).execute();
    if (!rd) { MESSAGE("nested note.read error: ",
                       note::to_string(rd.error())); }
    REQUIRE(rd);

    CHECK(recv.temp == doctest::Approx(sent.temp));
    CHECK(recv.pos.lat == doctest::Approx(sent.pos.lat));
    CHECK(recv.pos.lon == doctest::Approx(sent.pos.lon));
    if (recv.pos.lat == 0.0 && recv.pos.lon == 0.0) {
        MESSAGE("Nested 'pos' fields came back zero — Notecard likely "
                "flattened the template and pruned unknown fields.");
    } else {
        MESSAGE("Nested fields round-tripped — "
                "detail::notecard_supports_nested_templates_v can flip to true.");
    }

    nc.file.remove(file).execute();
}

TEST_CASE("note.template — does the Notecard accept ARRAY-OF-STRUCT templates?") {
    auto& nc = notecard_api();
    const char* file = "integration-tmpl-aostruct.qo";
    const char* noteId = "aos-rt";
    nc.file.remove(file).execute();

    auto reg = nc.note.templates().define(file)
        .body(note::body([](note::JsonBuilder& b) {
            b.begin_array("waypoints");
                b.begin_element_object();
                    b.add("lat", 14.1);
                    b.add("lon", 14.1);
                b.end_object();
            b.end_array();
        }))
        .execute();
    if (!reg) {
        MESSAGE("Notecard REJECTED array-of-struct template at registration: ",
                note::to_string(reg.error()));
        MESSAGE("→ keep the array-of-struct template branch gated off.");
        REQUIRE_FALSE(reg);
        nc.file.remove(file).execute();
        return;
    }
    MESSAGE("Array-of-struct template accepted at registration — "
            "probing round-trip.");

    TmplArrayOfStructs sent{{{{1.0, 2.0}, {3.0, 4.0}}}};
    auto add = nc.note.update(file, noteId).body(sent).execute();
    if (!add) { MESSAGE("aos note.update error: ",
                         note::to_string(add.error())); }
    REQUIRE(add);

    TmplArrayOfStructs recv{};
    auto rd = nc.note.read(file).noteId(noteId).into(recv).execute();
    if (!rd) { MESSAGE("aos note.read error: ",
                       note::to_string(rd.error())); }
    REQUIRE(rd);

    CHECK(recv.waypoints[0].lat == doctest::Approx(1.0));
    CHECK(recv.waypoints[0].lon == doctest::Approx(2.0));
    CHECK(recv.waypoints[1].lat == doctest::Approx(3.0));
    CHECK(recv.waypoints[1].lon == doctest::Approx(4.0));

    nc.file.remove(file).execute();
}

TEST_CASE("note.template array of primitives — known-good baseline") {
    auto& nc = notecard_api();
    const char* file = "integration-tmpl-aoprim.qo";
    nc.file.remove(file).execute();

    auto r = nc.note.templates().define(file)
        .body(note::body([](note::JsonBuilder& b) {
            b.begin_array("samples");
                b.add_element(note::json_int_t{12});
            b.end_array();
        }))
        .execute();
    if (!r) {
        MESSAGE("array-of-primitives template error: ",
                note::to_string(r.error()));
    }
    CHECK(r);
    nc.file.remove(file).execute();
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
    if (rsp) { MESSAGE("card.attn verify: ok"); }
    else { MESSAGE("card.attn verify: ", note::to_string(rsp.error())); }
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
    auto rsp = nc.note.add().file("test-fw-gate.qo").full(true).execute();
    if (!rsp) { MESSAGE("note.add full: ", note::to_string(rsp.error())); }
    CHECK(rsp);
    nc.file.remove("test-fw-gate.qo").execute();
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
    auto rsp = nc.note.add().file("test-fw-gate.qo").max(10).execute();
    if (!rsp) { MESSAGE("note.add max: ", note::to_string(rsp.error())); }
    CHECK(rsp);
    nc.file.remove("test-fw-gate.qo").execute();
}
} // fw>=8.2.1

// ─── ATTN intents ──────────────────────────────────────────────────────

TEST_CASE("card.attn arm + query + disarm round-trip") {
    auto& nc = notecard_api();

    auto arm_rsp = nc.card.attn().arm(note::attn::files).seconds(60).execute();
    if (!arm_rsp) { MESSAGE("arm error: ", note::to_string(arm_rsp.error())); }
    REQUIRE(arm_rsp);

    auto query_rsp = nc.card.attn().query().execute();
    if (!query_rsp) { MESSAGE("query error: ", note::to_string(query_rsp.error())); }
    REQUIRE(query_rsp);
    // set may be false if attention auto-fired (e.g. files already exist)
    MESSAGE("query set: ", query_rsp.set ? "true" : "false");

    auto disarm_rsp = nc.card.attn().disarm().execute();
    if (!disarm_rsp) { MESSAGE("disarm error: ", note::to_string(disarm_rsp.error())); }
    REQUIRE(disarm_rsp);
}

TEST_CASE("card.attn rearm is idempotent") {
    auto& nc = notecard_api();

    nc.card.attn().arm(note::attn::files).seconds(60).execute();
    auto r1 = nc.card.attn().rearm(note::attn::files).seconds(60).execute();
    if (!r1) { MESSAGE("rearm 1: ", note::to_string(r1.error())); }
    REQUIRE(r1);

    auto r2 = nc.card.attn().rearm(note::attn::files).seconds(60).execute();
    if (!r2) { MESSAGE("rearm 2: ", note::to_string(r2.error())); }
    REQUIRE(r2);

    nc.card.attn().disarm().execute();
}

TEST_CASE("card.attn off + on") {
    auto& nc = notecard_api();

    auto off_rsp = nc.card.attn().off().execute();
    if (!off_rsp) { MESSAGE("off error: ", note::to_string(off_rsp.error())); }
    REQUIRE(off_rsp);

    auto on_rsp = nc.card.attn().on().execute();
    if (!on_rsp) { MESSAGE("on error: ", note::to_string(on_rsp.error())); }
    REQUIRE(on_rsp);
}

// ─── Raw JSON passthrough ──────────────────────────────────────────────

TEST_CASE("transact(json, buf) returns valid JSON") {
    auto& nc = notecard_nc();
    char buf[512];
    auto rsp = nc.transact(R"({"req":"card.version"})", buf);
    if (!rsp) { MESSAGE("transact error: ", note::to_string(rsp.error())); }
    REQUIRE(rsp);
    MESSAGE("transact buf response (", rsp->size(), " bytes): [", rsp->data(), "]");
    CHECK(rsp->find("version") != note::string_view::npos);
}

TEST_CASE("transact(json) auto-sizes response") {
    auto& nc = notecard_nc();
    auto rsp = nc.transact(R"({"req":"card.version"})");
    if (!rsp) { MESSAGE("transact error: ", note::to_string(rsp.error())); }
    REQUIRE(rsp);
    CHECK(rsp->view().find("version") != note::string_view::npos);
}

// ─── ping ──────────────────────────────────────────────────────────────

TEST_CASE("ping confirms connectivity to the live Notecard") {
    auto& nc = notecard_nc();
    auto rv = nc.ping();
    if (!rv) { MESSAGE("ping error: ", note::to_string(rv.error())); }
    REQUIRE(rv);
}

TEST_CASE("ping is repeatable across consecutive calls") {
    auto& nc = notecard_nc();
    auto a = nc.ping();
    auto b = nc.ping();
    auto c = nc.ping();
    if (!a) { MESSAGE("first ping error: ",  note::to_string(a.error())); }
    if (!b) { MESSAGE("second ping error: ", note::to_string(b.error())); }
    if (!c) { MESSAGE("third ping error: ",  note::to_string(c.error())); }
    CHECK(a);
    CHECK(b);
    CHECK(c);
}

TEST_CASE("ping with custom timeout") {
    auto& nc = notecard_nc();
    auto rv = nc.ping(2000);
    if (!rv) { MESSAGE("ping(2000) error: ", note::to_string(rv.error())); }
    REQUIRE(rv);
}

TEST_CASE("ping leaves typed requests working afterwards") {
    auto& nc = notecard_nc();
    auto& api = notecard_api();
    auto rv = nc.ping();
    REQUIRE(rv);
    auto ver = api.card.version().execute();
    if (!ver) { MESSAGE("card.version after ping error: ", note::to_string(ver.error())); }
    REQUIRE(ver);
    CHECK(!note::string_view(ver.version).empty());
}

TEST_CASE("send(json) fire-and-forget") {
    auto& nc = notecard_nc();
    auto r = nc.send(R"({"cmd":"hub.set","product":"com.example.integration-test"})");
    if (!r) { MESSAGE("send error: ", note::to_string(r.error())); }
    REQUIRE(r);
}

// ─── Debug output ──────────────────────────────────────────────────────

TEST_CASE("debug wire output captures request and response") {
    auto& nc = notecard_nc();

    bool saw_send = false;
    bool saw_receive = false;
    note::DebugListener d;
    d.ctx = &saw_send;  // reuse for both
    d.on_wire = [](const note::WireEvent& ev, void* ctx) {
        if (ev.direction == note::WireDirection::Send) *static_cast<bool*>(ctx) = true;
    };
    // Capture receive in a separate way — use a struct
    struct Ctx { bool* send; bool recv = false; };
    Ctx debug_ctx{&saw_send};
    d.ctx = &debug_ctx;
    d.on_wire = [](const note::WireEvent& ev, void* ctx) {
        auto* c = static_cast<Ctx*>(ctx);
        if (ev.direction == note::WireDirection::Send) *c->send = true;
        if (ev.direction == note::WireDirection::Receive) c->recv = true;
    };
    nc.set_debug(d);

    nc.execute(note::api::CardVersion{});

    CHECK(saw_send);
    CHECK(debug_ctx.recv);
    nc.clear_debug();
}

TEST_SUITE("fw>=9.1.1") {
TEST_CASE("note.add limit field (9.1.1+)") {
    auto& nc = notecard_api();
    // The `limit` field on note.add is a boolean (per OpenAPI spec): when
    // true, the Notecard refuses to create the Note if it's in a penalty
    // box. We're testing that the *field* is accepted by 9.1.1+ firmware,
    // not the penalty-box behavior — so set mode:off first to take Notehub
    // sync state out of the equation, then restore.
    auto saved_mode = nc.hub.get().execute();
    nc.hub.set().mode("off").execute();
    nc.file.remove("test-fw-gate.qo").execute();
    auto rsp = nc.note.add().file("test-fw-gate.qo").limit(true).execute();
    if (!rsp) { MESSAGE("note.add limit: ", note::to_string(rsp.error())); }
    CHECK(rsp);
    nc.file.remove("test-fw-gate.qo").execute();
    if (saved_mode) {
        nc.hub.set().mode(std::string(saved_mode.mode)).execute();
    }
}
} // fw>=9.1.1
