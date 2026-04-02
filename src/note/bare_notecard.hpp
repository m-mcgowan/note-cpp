#pragma once

/// @file bare_notecard.hpp
/// BareNotecard — raw JSON passthrough over a Notecard transport.
///
/// Sends and receives pre-formatted JSON strings without parsing or
/// typed response handling. Useful for:
/// - Serial passthrough protocols (forwarding external JSON to the Notecard)
/// - Debug consoles
/// - Bridge firmware that relays arbitrary requests
///
/// No allocator, no JSON backend, no typed API. Just validated JSON in/out.
///
/// Usage:
///   note::StreamingTransport transport(hal);
///   note::BareNotecard bare(transport);
///
///   char buf[512];
///   auto rsp = bare.transact(R"({"req":"card.version"})", buf);
///   bare.send(R"({"cmd":"hub.set","product":"com.example"})");

#include "error.hpp"
#include "json_sax.hpp"
#include "owned_buffer.hpp"
#include "span.hpp"
#include "streaming_transport.hpp"
#include "types.hpp"

namespace note {

class BareNotecard {
public:
    explicit BareNotecard(StreamingTransport& transport)
        : transport_(transport) {}

    /// Send pre-formatted JSON request, read response into caller's buffer.
    /// The JSON is validated (SAX-parsed) before sending. The response is
    /// raw bytes from the Notecard — no parsing or reconstruction.
    Result<string_view> transact(string_view json, span<char> buf) {
        if (!validate(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));
        return transport_.transact_raw(json, buf.data(), buf.size(), timeout_ms_);
    }

    /// Send pre-formatted JSON command (fire-and-forget).
    /// The JSON is validated before sending.
    Result<void> send(string_view json) {
        if (!validate(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));
        return transport_.send_raw(json);
    }

    void set_timeout(uint32_t ms) { timeout_ms_ = ms; }
    uint32_t timeout() const { return timeout_ms_; }

private:
    static bool validate(string_view json) {
        JsonSink null_sink;
        return sax_parse(json, null_sink).empty();
    }

    StreamingTransport& transport_;
    uint32_t timeout_ms_ = 10000;
};

} // namespace note
