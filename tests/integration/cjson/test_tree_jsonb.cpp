// Tree × JSONB parity test.
//
// Verifies that the wire-format axis and the response-presentation axis
// compose orthogonally. The Notecard architecture exposes:
//
//   wire format   : JSON text  ⟷  JSONB binary opcodes
//   presentation  : streaming  ⟷  tree (JsonReader)
//
// This test exercises the previously-broken cell: JSONB wire feeding a
// tree-mode backend. The integration point is the SAX event stream —
// both wire parsers emit the same SaxEvent shape, and the JsonBackend's
// start_response/finish_response surface consumes those events into a
// tree regardless of which parser produced them.
//
// We feed canned JSONB binary into the JSONB parser, route its SAX
// events into CjsonBackend's tree-builder sink, and assert the
// resulting CjsonReader yields the expected fields. Compared against
// a JSON-text path parsed through the same backend, the readers must
// be equivalent — proving the two axes are orthogonal.

#include <doctest.h>

#include <note/backends/cjson.hpp>
#include <note/jsonb.hpp>
#include <note/lexer/sax_adapter.hpp>

#include <cstdint>
#include <vector>

using namespace note::backends;

namespace {

// Generate canned JSONB bytes for a representative response shape via
// the production StreamingJsonbBuilder. Mirrors what JsonbRequestTransport
// would emit on the wire (minus the {: ... :} envelope, which is the
// transport's framing, not the body).
struct ByteSinkWriter : note::JsonWriter {
    using note::JsonWriter::write;
    std::vector<uint8_t> bytes;
    bool write(const char* data, size_t len) override {
        bytes.insert(bytes.end(),
                     reinterpret_cast<const uint8_t*>(data),
                     reinterpret_cast<const uint8_t*>(data) + len);
        return true;
    }
};

// Replay byte buffer back into a ReadFn for jsonb_parse_streaming.
struct ByteReplay {
    const uint8_t* data;
    size_t len;
    size_t pos = 0;

    note::Result<size_t> operator()(uint8_t* out, size_t max, uint32_t /*t*/) {
        if (pos >= len) return note::make_error(note::Error::EndOfFrame, "");
        size_t take = (len - pos) < max ? (len - pos) : max;
        for (size_t i = 0; i < take; ++i) out[i] = data[pos + i];
        pos += take;
        return take;
    }
};

} // namespace

TEST_CASE("tree_jsonb/parity_with_json_text") {
    // Build the same response shape two ways and assert reader parity.
    //
    //   JSON text  → CjsonBackend::parse_response   → reader_a
    //   JSONB bin  → CjsonBackend::start_response   → reader_b
    //                  (driven by jsonb_parse_streaming)

    const char* json_text = R"({"req":"card.status","id":42,"ok":true,"temp":22.5,"net":{"signal":-65,"name":"my-net"}})";

    // ── Wire format A: JSON text → tree ───────────────────────────────
    CjsonBackend backend_a;
    auto reader_a = backend_a.parse_response(json_text);
    REQUIRE(reader_a);
    REQUIRE_FALSE(reader_a->has_error());

    // ── Wire format B: JSONB binary → tree ────────────────────────────
    // Build the JSONB byte stream via StreamingJsonbBuilder so the test
    // mirrors exactly what the transport emits on the wire.
    ByteSinkWriter w;
    {
        note::StreamingJsonbBuilder b(w);
        b.add("req", "card.status");
        b.add("id",  note::json_int_t{42});
        b.add("ok",  true);
        b.add("temp", 22.5);
        b.begin_object("net");
        b.add("signal", note::json_int_t{-65});
        b.add("name",   "my-net");
        b.end_object();
    }
    // Close the root object — the transport's emit_ would write the
    // kEndObject opcode here.
    uint8_t end = note::jsonb::kEndObject;
    w.write(reinterpret_cast<const char*>(&end), 1);

    CjsonBackend backend_b;
    char unused[1];  // cjson direct sink ignores the work_buf.
    note::JsonSink& sink = backend_b.start_response(
        note::span<char>(unused, sizeof(unused)));

    auto dispatch = note::make_sax_dispatch(sink);
    ByteReplay replay{w.bytes.data(), w.bytes.size()};
    char sax_buf[256];
    note::SaxStreamBuf sb(sax_buf);
    auto err = note::jsonb_parse_streaming(replay, 1000, sb, dispatch);
    REQUIRE(err.empty());

    note::JsonReader& reader_b = backend_b.finish_response();
    REQUIRE_FALSE(reader_b.has_error());

    // ── Parity assertions ─────────────────────────────────────────────
    CHECK(reader_a->get_string("req") == reader_b.get_string("req"));
    CHECK(reader_a->get_int("id")     == reader_b.get_int("id"));
    CHECK(reader_a->get_bool("ok")    == reader_b.get_bool("ok"));
    CHECK(reader_a->get_double("temp") == reader_b.get_double("temp"));

    auto net_a = reader_a->get_object("net");
    auto net_b = reader_b.get_object("net");
    REQUIRE(net_a);
    REQUIRE(net_b);
    CHECK(net_a->get_int("signal")    == net_b->get_int("signal"));
    CHECK(net_a->get_string("name")   == net_b->get_string("name"));
}

TEST_CASE("tree_jsonb/error_field_surfaces_through_jsonb_path") {
    // Notecard errors arrive in the response's "err" field. Verify the
    // JSONB → tree path surfaces them via JsonReader::get_error().

    ByteSinkWriter w;
    {
        note::StreamingJsonbBuilder b(w);
        b.add("err", "{io}");
    }
    uint8_t end = note::jsonb::kEndObject;
    w.write(reinterpret_cast<const char*>(&end), 1);

    CjsonBackend backend;
    char unused[1];
    note::JsonSink& sink = backend.start_response(
        note::span<char>(unused, sizeof(unused)));

    auto dispatch = note::make_sax_dispatch(sink);
    ByteReplay replay{w.bytes.data(), w.bytes.size()};
    char sax_buf[256];
    note::SaxStreamBuf sb(sax_buf);
    auto perr = note::jsonb_parse_streaming(replay, 1000, sb, dispatch);
    REQUIRE(perr.empty());

    note::JsonReader& reader = backend.finish_response();
    CHECK(reader.get_error() == "{io}");
}

TEST_CASE("tree_jsonb/release_response_survives_next_transaction") {
    // The no-allocator error path in execute_tree depends on
    // release_response transferring the tree to an owning unique_ptr
    // so the err string outlives subsequent transactions. Verify that
    // contract here directly.

    CjsonBackend backend;
    char unused[1];

    {
        note::JsonSink& s = backend.start_response(
            note::span<char>(unused, sizeof(unused)));
        s.on_object_begin("");
        s.on_string("err", "first error");
        s.on_object_end("");
    }
    auto owned = backend.release_response();
    REQUIRE(owned);
    CHECK(owned->get_error() == "first error");

    // Run a second transaction — would clobber a non-owning reader.
    {
        note::JsonSink& s = backend.start_response(
            note::span<char>(unused, sizeof(unused)));
        s.on_object_begin("");
        s.on_string("err", "second error");
        s.on_object_end("");
        (void)backend.finish_response();
    }

    // The released reader's error must still be valid — it owns the tree.
    CHECK(owned->get_error() == "first error");
}
