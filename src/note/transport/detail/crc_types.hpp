#pragma once

/// @file crc_types.hpp
/// CRC-related types shared by streaming and buffered transport paths.

#include <note/json.hpp>
#include <note/transport/detail/crc32.hpp>

namespace note {

/// JsonWriter that accumulates CRC32 and forwards to an inner writer.
struct CrcWriter : JsonWriter {
    using JsonWriter::write;
    JsonWriter& inner;
    uint32_t state;

    explicit CrcWriter(JsonWriter& w)
        : inner(w), state(transport::detail::crc32_begin()) {}

    bool write(const char* data, size_t len) override {
        state = transport::detail::crc32_update(state, data, len);
        return inner.write(data, len);
    }

    /// Feed '}' to CRC without writing it, return finalized checksum.
    uint32_t finalize_with_brace() {
        state = transport::detail::crc32_update(state, "}", 1);
        return transport::detail::crc32_finalize(state);
    }
};

/// SAX sink filter that intercepts the "crc" field from a response.
/// Extracts sequence number and checksum; does not forward to inner sink.
struct CrcFieldSink : FilterJsonSink {
    uint16_t seq_ = 0;
    uint32_t checksum_ = 0;
    bool found_ = false;

    explicit CrcFieldSink(JsonSink& inner) : FilterJsonSink(inner) {}

    bool has_crc() const { return found_; }
    uint16_t seq() const { return seq_; }
    uint32_t checksum() const { return checksum_; }

    void on_string(string_view key, string_view value) override {
        if (key == "crc") {
            if (value.size() == 13 && value[4] == ':') {
                seq_ = static_cast<uint16_t>(
                    transport::detail::read_hex(value.data(), 4));
                checksum_ = static_cast<uint32_t>(
                    transport::detail::read_hex(value.data() + 5, 8));
                found_ = true;
            }
            return;
        }
        inner_.on_string(key, value);
    }

    void reset() override {
        seq_ = 0;
        checksum_ = 0;
        found_ = false;
        FilterJsonSink::reset();
    }
};

} // namespace note
