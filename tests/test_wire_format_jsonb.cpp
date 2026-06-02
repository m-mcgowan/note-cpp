/// @file test_wire_format_jsonb.cpp
/// JSONB-wire counterpart to test_wire_format.cpp.
///
/// Where test_wire_format.cpp asserts on JSON text shape (`last_request ==
/// R"({"req":"hub.set",…})"`) — and is therefore JSONB-incompatible — this
/// file asserts on the *opcode* shape of the same requests. The pre-COBS,
/// pre-envelope opcode stream is what the JSONB wire format actually carries
/// (the COBS encoding and `{:...:}` envelope are stateless transforms
/// covered by test_jsonb.cpp's primitive tests).
///
/// These tests do NOT require the build to define NOTE_JSONB — the JSONB
/// encoder is always present in the library, regardless of which wire
/// format the transport is configured to use. They exercise the encoder
/// directly via test::build_jsonb_request<Req>().

#include <doctest.h>

#include "common/jsonb_request_builder.hpp"

#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/env_set.hpp>
#include <note/api/note_get.hpp>
#include <note/api/note_add.hpp>
#include <note/api/file_delete.hpp>
#include <note/api/card_attn.hpp>

using namespace note;
using note::test::build_jsonb_request;
using note::test::find_jsonb_string;
using note::test::find_jsonb_int32;
using note::test::find_jsonb_bool;

// ---------------------------------------------------------------------------
// Wire-shape primitives — the framing invariants every request must hit.
// ---------------------------------------------------------------------------

TEST_CASE("jsonb wire: request stream begins with kBeginObject, ends with kEndObject") {
    note::api::CardVersion req;
    auto bytes = build_jsonb_request(req);
    REQUIRE(bytes.size() >= 2);
    CHECK(bytes.front() == jsonb::kBeginObject);
    CHECK(bytes.back()  == jsonb::kEndObject);
}

// ---------------------------------------------------------------------------
// Simple endpoints — direct ports of test_wire_format.cpp's TEST_CASEs.
// ---------------------------------------------------------------------------

TEST_CASE("jsonb wire: CardVersion produces minimal request") {
    // Equivalent JSON-shape assertion:
    //   last_request == R"({"req":"card.version"})"
    note::api::CardVersion req;
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req") == "card.version");

    // No other fields: stream is kBeginObject + kItem "req" + kString "card.version" + kEndObject.
    const uint8_t expected[] = {
        jsonb::kBeginObject,
        jsonb::kItem,   'r', 'e', 'q', 0,
        jsonb::kString, 'c', 'a', 'r', 'd', '.', 'v', 'e', 'r', 's', 'i', 'o', 'n', 0,
        jsonb::kEndObject,
    };
    REQUIRE(bytes.size() == sizeof(expected));
    CHECK(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0);
}

TEST_CASE("jsonb wire: HubSet with product and mode") {
    // Equivalent JSON-shape assertion:
    //   last_request == R"({"req":"hub.set","mode":"periodic","product":"com.example.test"})"
    note::api::HubSet req;
    req.product("com.example.test").mode("periodic");
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req")     == "hub.set");
    CHECK(find_jsonb_string(bytes, "mode")    == "periodic");
    CHECK(find_jsonb_string(bytes, "product") == "com.example.test");
}

TEST_CASE("jsonb wire: HubSet with no fields emits only req") {
    note::api::HubSet req;
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req") == "hub.set");
    // No "mode", no "product".
    CHECK(find_jsonb_string(bytes, "mode").empty());
    CHECK(find_jsonb_string(bytes, "product").empty());
}

TEST_CASE("jsonb wire: HubSet with integer field") {
    // Equivalent JSON-shape assertion:
    //   last_request == R"({"req":"hub.set","outbound":60})"
    note::api::HubSet req;
    req.outbound(60);
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req") == "hub.set");
    int32_t outbound = 0;
    REQUIRE(find_jsonb_int32(bytes, "outbound", outbound));
    CHECK(outbound == 60);
}

TEST_CASE("jsonb wire: EnvSet with string fields") {
    // Equivalent JSON-shape assertion:
    //   last_request == R"({"req":"env.set","name":"temperature","text":"22.5"})"
    note::api::EnvSet req;
    req.name = "temperature";
    req.text("22.5");
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req")  == "env.set");
    CHECK(find_jsonb_string(bytes, "name") == "temperature");
    CHECK(find_jsonb_string(bytes, "text") == "22.5");
}

// ---------------------------------------------------------------------------
// Polymorphic endpoints — dispatch shape under JSONB.
// ---------------------------------------------------------------------------

TEST_CASE("jsonb wire: NoteGet::Get excludes delete property") {
    note::api::NoteGet::Get req;
    req.file("data.qi");
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req")  == "note.get");
    CHECK(find_jsonb_string(bytes, "file") == "data.qi");
    bool del = true;
    CHECK_FALSE(find_jsonb_bool(bytes, "delete", del));
}

TEST_CASE("jsonb wire: NoteGet::Delete includes delete:true") {
    note::api::NoteGet::Delete req;
    req.file("requests.qi");
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req")  == "note.get");
    CHECK(find_jsonb_string(bytes, "file") == "requests.qi");
    bool del = false;
    REQUIRE(find_jsonb_bool(bytes, "delete", del));
    CHECK(del == true);
}

TEST_CASE("jsonb wire: NoteGet::Get with note_id uses wire name 'note'") {
    note::api::NoteGet::Get req;
    req.file("settings.db").noteId("config");
    auto bytes = build_jsonb_request(req);

    CHECK(find_jsonb_string(bytes, "req")  == "note.get");
    CHECK(find_jsonb_string(bytes, "file") == "settings.db");
    CHECK(find_jsonb_string(bytes, "note") == "config");
}

// ---------------------------------------------------------------------------
// id propagation (request_id != 0) — framed_build's optional prefix.
// ---------------------------------------------------------------------------

TEST_CASE("jsonb wire: request id appears as kItem 'id' kInt32 <n>") {
    note::api::CardVersion req;
    auto bytes = build_jsonb_request(req, /*req_id=*/42);

    CHECK(find_jsonb_string(bytes, "req") == "card.version");
    int32_t id = 0;
    REQUIRE(find_jsonb_int32(bytes, "id", id));
    CHECK(id == 42);
}

TEST_CASE("jsonb wire: request id 0 is omitted from the wire") {
    note::api::CardVersion req;
    auto bytes = build_jsonb_request(req, /*req_id=*/0);

    int32_t id = -1;
    CHECK_FALSE(find_jsonb_int32(bytes, "id", id));
}
