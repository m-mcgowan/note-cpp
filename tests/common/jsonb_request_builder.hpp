#pragma once

/// @file jsonb_request_builder.hpp
/// Test helpers for capturing JSONB request opcode streams.
///
/// The full wire path under NOTE_JSONB is:
///
///   src.emit(StreamingJsonbBuilder over CobsStreamWriter over IByteTransport)
///   ↓
///   `{:` envelope + COBS-encoded opcode stream + `:}` envelope + line terminator
///
/// These helpers capture at the StreamingJsonbBuilder layer — the raw opcode
/// stream BEFORE COBS encoding and BEFORE envelope framing. The COBS/envelope
/// path is covered by tests in test_jsonb.cpp (CobsStreamWriter unit tests)
/// and on hardware via the HIL JSONB envs.
///
/// What this layer exercises:
///   - Typed request structs produce the right "req":"<op>" + field opcodes
///   - Required fields are present
///   - Field types map to the right JSONB opcodes (kString/kInt32/kDouble/...)
///   - Field values round-trip correctly
///
/// What this layer does NOT exercise (covered elsewhere):
///   - COBS encoding/decoding of the opcode stream
///   - `{:...:}` envelope framing
///   - IByteTransport write batching
///   - Full Notecard execute() path (errors, retries, debug-wire)

#include <note/generic_builder.hpp>
#include <note/jsonb.hpp>
#include <note/json_sax.hpp>
#include <note/notecard.hpp>

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace note::test {

/// Captures the byte stream emitted by a JsonWriter — the substrate beneath
/// StreamingJsonbBuilder. Each `write()` call appends to `bytes`.
struct ByteCapture : note::JsonWriter {
    std::vector<uint8_t> bytes;

    bool write(const char* data, size_t len) override {
        for (size_t i = 0; i < len; ++i)
            bytes.push_back(static_cast<uint8_t>(data[i]));
        return true;
    }
};

/// Build the JSONB opcode stream for a typed request, replicating the shape
/// `Notecard::framed_build` produces on the wire. Mirrors the prefix that
/// `framed_build` prepends:
///
///   kBeginObject
///   kItem "req\0" kString "<op-name>\0"
///   [kItem "id\0" kInt32 <id>]            // only when req_id != 0
///   <implicit-fields from extras callback (e.g. delete:true on Pop intent)>
///   <fields from generic_build(b, req, descs)>
///   kEndObject
///
/// Reaches the per-endpoint field set via the public `request_traits<Req>` +
/// `generic_build` path — same primitives the codegen-generated `Req::build()`
/// uses internally. Polymorphic-intent implicit fields are added in
/// `Req::build()` *around* the `generic_build` call, not in the field-desc
/// table; pass an `extras` callable to inject them in the same place.
/// Friend-only test probe — opens the private `Req::build(JsonBuilder&)` to
/// the request-builder helpers below. The codegen `endpoint.hpp.j2` template
/// declares `friend struct ::note::test::JsonbWireProbe;` on every request
/// struct so this is the single chokepoint that bypasses encapsulation
/// (production paths still go through `Notecard::execute()` / `command()`).
struct JsonbWireProbe {
    template<typename Req>
    static void invoke_build(const Req& req, note::JsonBuilder& b) {
        req.build(b);
    }
};

/// Build the JSONB opcode stream for a typed request, replicating the shape
/// `Notecard::framed_build` produces on the wire:
///
///   kBeginObject
///   kItem "req\0" kString "<op-name>\0"
///   [kItem "id\0" kInt32 <id>]            // only when req_id != 0
///   <fields from req.build(b)>            // delegates to the codegen-generated
///   kEndObject                            //   per-endpoint Req::build()
///
/// Use to assert per-endpoint wire format under NOTE_JSONB without standing
/// up a full Notecard + transport. Calls `req.build()` directly via
/// `JsonbWireProbe` so manually-emitted fields (e.g. `delete:true` on
/// `NoteGet::Pop`, `name` on `EnvSet`) appear in the output.
template<typename Req>
std::vector<uint8_t> build_jsonb_request(const Req& req, uint32_t req_id = 0) {
    ByteCapture cap;
    note::StreamingJsonbBuilder b(cap);
    b.add("req", Req::notecard_request);
    if (req_id)
        b.add("id", static_cast<note::json_int_t>(req_id));
    JsonbWireProbe::invoke_build(req, b);
    b.to_view();
    return std::move(cap.bytes);
}

/// Search for `kItem <key>\0 kString <out>\0` in the opcode stream and
/// return a string_view into `bytes` for the string value. Returns an
/// empty view if the key is missing or its value isn't a kString.
inline note::string_view find_jsonb_string(const std::vector<uint8_t>& bytes,
                                           note::string_view key) {
    const size_t klen = key.size();
    for (size_t i = 0; i + 1 + klen + 1 < bytes.size(); ++i) {
        if (bytes[i] != note::jsonb::kItem) continue;
        if (memcmp(&bytes[i + 1], key.data(), klen) != 0) continue;
        if (bytes[i + 1 + klen] != 0) continue;
        // Past the item header, look for kString.
        size_t v = i + 1 + klen + 1;
        if (v >= bytes.size() || bytes[v] != note::jsonb::kString) return {};
        ++v;
        // Find the value terminator.
        size_t end = v;
        while (end < bytes.size() && bytes[end] != 0) ++end;
        if (end >= bytes.size()) return {};
        return note::string_view(reinterpret_cast<const char*>(&bytes[v]),
                                 end - v);
    }
    return {};
}

/// Search for `kItem <key>\0 kInt32 <4 bytes LE>` and decode the int.
/// Returns true with the value out-parameter set on success.
inline bool find_jsonb_int32(const std::vector<uint8_t>& bytes,
                             note::string_view key,
                             int32_t& out) {
    const size_t klen = key.size();
    for (size_t i = 0; i + 1 + klen + 1 + 4 < bytes.size(); ++i) {
        if (bytes[i] != note::jsonb::kItem) continue;
        if (memcmp(&bytes[i + 1], key.data(), klen) != 0) continue;
        if (bytes[i + 1 + klen] != 0) continue;
        size_t v = i + 1 + klen + 1;
        if (bytes[v] != note::jsonb::kInt32) continue;
        ++v;
        uint32_t u = uint32_t(bytes[v])
                   | (uint32_t(bytes[v + 1]) << 8)
                   | (uint32_t(bytes[v + 2]) << 16)
                   | (uint32_t(bytes[v + 3]) << 24);
        out = static_cast<int32_t>(u);
        return true;
    }
    return false;
}

/// True if `kItem <key>\0 kTrue|kFalse` appears in the stream.
inline bool find_jsonb_bool(const std::vector<uint8_t>& bytes,
                            note::string_view key,
                            bool& out) {
    const size_t klen = key.size();
    for (size_t i = 0; i + 1 + klen + 1 < bytes.size(); ++i) {
        if (bytes[i] != note::jsonb::kItem) continue;
        if (memcmp(&bytes[i + 1], key.data(), klen) != 0) continue;
        if (bytes[i + 1 + klen] != 0) continue;
        uint8_t op = bytes[i + 1 + klen + 1];
        if (op == note::jsonb::kTrue) { out = true; return true; }
        if (op == note::jsonb::kFalse) { out = false; return true; }
        return false;
    }
    return false;
}

}  // namespace note::test
