#pragma once
/// @file jsonb.hpp
/// JSONB binary wire format — opcode constants and streaming builder.
///
/// JSONB is Blues' TLV binary encoding for Notecard communication.
/// Each value is an opcode byte followed by its payload. Strings are
/// null-terminated; integers and floats are little-endian fixed width.
///
/// The builder implements JsonBuilder and writes opcodes through a
/// JsonWriter (which may be backed by a COBS encoder in the streaming
/// transport path).

#include "json.hpp"
#include "json_sax_streaming.hpp"
#include "lexer/sax_adapter.hpp"
#include "transport/cobs.hpp"

#include <cstdint>
#include <cstring>

namespace note {

// ---------------------------------------------------------------------------
// JSONB opcode constants
// ---------------------------------------------------------------------------

namespace jsonb {

constexpr uint8_t kBeginObject = 0x10;
constexpr uint8_t kEndObject   = 0x11;
constexpr uint8_t kBeginArray  = 0x12;
constexpr uint8_t kEndArray    = 0x13;

constexpr uint8_t kNull        = 0x20;
constexpr uint8_t kTrue        = 0x21;
constexpr uint8_t kFalse       = 0x22;

constexpr uint8_t kItem        = 0x30;  // field name (null-terminated)
constexpr uint8_t kString      = 0x40;  // string value (null-terminated)

constexpr uint8_t kInt8        = 0x61;
constexpr uint8_t kInt16       = 0x62;
constexpr uint8_t kInt32       = 0x64;
constexpr uint8_t kInt64       = 0x68;

constexpr uint8_t kUint8       = 0x71;
constexpr uint8_t kUint16      = 0x72;
constexpr uint8_t kUint32      = 0x74;
constexpr uint8_t kUint64      = 0x78;

constexpr uint8_t kFloat       = 0x84;
constexpr uint8_t kDouble      = 0x88;

/// COBS XOR byte for JSONB framing (eliminates newlines from payload).
constexpr uint8_t kCobsXor     = '\n';

}  // namespace jsonb

// ---------------------------------------------------------------------------
// StreamingJsonbBuilder — JsonBuilder that emits JSONB opcodes through a
// JsonWriter.
//
// Same streaming architecture as StreamingJsonBuilder: the constructor
// emits kBeginObject, each add() writes opcodes directly. The transport
// handles framing ({: header, COBS encoding, :}\n trailer) and closing
// the root object (kEndObject).
//
// to_view() is for standalone use only — it closes with kEndObject.
// ---------------------------------------------------------------------------

class StreamingJsonbBuilder : public JsonBuilder {
public:
    explicit StreamingJsonbBuilder(JsonWriter& w) : writer_(w) {
        emit(jsonb::kBeginObject);
    }

    JsonBuilder& add(string_view key, bool value) override {
        emit_item(key);
        emit(value ? jsonb::kTrue : jsonb::kFalse);
        return *this;
    }

    JsonBuilder& add(string_view key, int32_t value) override {
        emit_item(key);
        emit(jsonb::kInt32);
        emit_le32(value);
        return *this;
    }

    JsonBuilder& add(string_view key, double value) override {
        emit_item(key);
        emit(jsonb::kDouble);
        emit_bytes(&value, 8);
        return *this;
    }

    JsonBuilder& add(string_view key, string_view value) override {
        emit_item(key);
        emit(jsonb::kString);
        writer_.write(value.data(), value.size());
        emit('\0');
        return *this;
    }

    JsonBuilder& add_raw(string_view, string_view) override {
        // No-op — raw JSON fragments cannot be embedded in JSONB.
        // BodyValue's raw-string constructor is disabled when NOTE_JSONB=1
        // so this path is unreachable in normal use.
        return *this;
    }

    JsonBuilder& begin_object(string_view key) override {
        emit_item(key);
        emit(jsonb::kBeginObject);
        return *this;
    }

    JsonBuilder& end_object() override {
        emit(jsonb::kEndObject);
        return *this;
    }

    JsonBuilder& begin_array(string_view key) override {
        emit_item(key);
        emit(jsonb::kBeginArray);
        return *this;
    }

    JsonBuilder& end_array() override {
        emit(jsonb::kEndArray);
        return *this;
    }

    // Array elements — no kItem prefix.
    JsonBuilder& add_element(bool value) override {
        emit(value ? jsonb::kTrue : jsonb::kFalse);
        return *this;
    }

    JsonBuilder& add_element(int32_t value) override {
        emit(jsonb::kInt32);
        emit_le32(value);
        return *this;
    }

    JsonBuilder& add_element(double value) override {
        emit(jsonb::kDouble);
        emit_bytes(&value, 8);
        return *this;
    }

    JsonBuilder& add_element(string_view value) override {
        emit(jsonb::kString);
        writer_.write(value.data(), value.size());
        emit('\0');
        return *this;
    }

    string_view to_view() override {
        if (!closed_) {
            emit(jsonb::kEndObject);
            closed_ = true;
        }
        return {};
    }

    void reset() override {
        closed_ = false;
        emit(jsonb::kBeginObject);
    }

private:
    JsonWriter& writer_;
    bool closed_ = false;

    void emit(uint8_t opcode) {
        writer_.write(reinterpret_cast<const char*>(&opcode), 1);
    }

    void emit_bytes(const void* data, size_t len) {
        writer_.write(reinterpret_cast<const char*>(data), len);
    }

    void emit_le32(int32_t value) {
        uint8_t le[4];
        auto uval = static_cast<uint32_t>(value);
        le[0] = static_cast<uint8_t>(uval);
        le[1] = static_cast<uint8_t>(uval >> 8);
        le[2] = static_cast<uint8_t>(uval >> 16);
        le[3] = static_cast<uint8_t>(uval >> 24);
        emit_bytes(le, 4);
    }

    void emit_item(string_view key) {
        emit(jsonb::kItem);
        writer_.write(key.data(), key.size());
        emit('\0');
    }
};

// ---------------------------------------------------------------------------
// CobsStreamWriter — JsonWriter that COBS-encodes bytes as they're written.
//
// Processes bytes one at a time through a 255-byte block buffer. When a
// zero byte is encountered or the block fills (code=0xFF), the block is
// flushed to the inner writer. Call flush() after the last write to emit
// the final block.
// ---------------------------------------------------------------------------

class CobsStreamWriter : public JsonWriter {
public:
    using JsonWriter::write;

    CobsStreamWriter(JsonWriter& inner, uint8_t xor_byte)
        : inner_(inner), xor_(xor_byte) {}

    bool write(const char* data, size_t len) override {
        for (size_t i = 0; i < len; ++i) {
            auto byte = static_cast<uint8_t>(data[i]);
            if (byte == 0) {
                if (!flush_block()) return false;
            } else {
                block_[block_len_++] = byte ^ xor_;
                ++code_;
                if (code_ == 0xFF) {
                    if (!flush_block()) return false;
                }
            }
        }
        return true;
    }

    bool flush() { return flush_block(); }

private:
    JsonWriter& inner_;
    uint8_t xor_;
    uint8_t block_[255]{};
    size_t block_len_ = 1;  // slot 0 reserved for code byte
    uint8_t code_ = 1;

    bool flush_block() {
        block_[0] = code_ ^ xor_;
        bool ok = inner_.write(reinterpret_cast<const char*>(block_), block_len_);
        code_ = 1;
        block_len_ = 1;
        return ok;
    }
};

// ---------------------------------------------------------------------------
// CobsDecodingReader — ReadFn adapter that COBS-decodes bytes from an
// inner ReadFn. Reads encoded chunks, decodes them, and returns decoded
// bytes to the caller. Also strips the `:}` JSONB trailer.
// ---------------------------------------------------------------------------

namespace detail {

template<typename ReadFn>
class CobsDecodingReader {
public:
    CobsDecodingReader(ReadFn& inner, uint32_t timeout_ms)
        : inner_(inner), timeout_ms_(timeout_ms), decoder_(jsonb::kCobsXor) {}

    Result<size_t> operator()(uint8_t* buf, size_t max, uint32_t /*timeout*/) {
        // Return any buffered decoded bytes first.
        if (dec_pos_ < dec_len_) {
            size_t n = max < (dec_len_ - dec_pos_) ? max : (dec_len_ - dec_pos_);
            memcpy(buf, dec_buf_ + dec_pos_, n);
            dec_pos_ += n;
            return n;
        }

        if (done_) return size_t(0);

        // Read encoded bytes from the wire.
        uint8_t enc[64];
        auto r = inner_(enc, sizeof(enc), timeout_ms_);
        if (!r) return r;
        if (*r == 0) { done_ = true; return size_t(0); }

        // Strip `:}` trailer if present at the end of the chunk.
        size_t enc_len = *r;
        if (enc_len >= 2 &&
            enc[enc_len - 2] == ':' && enc[enc_len - 1] == '}') {
            enc_len -= 2;
            done_ = true;
        }
        if (enc_len == 0) return size_t(0);

        // COBS-decode into the decode buffer.
        dec_len_ = 0;
        dec_pos_ = 0;
        auto out = [this](const uint8_t* data, size_t n) {
            size_t copy = n;
            if (dec_len_ + copy > sizeof(dec_buf_))
                copy = sizeof(dec_buf_) - dec_len_;
            memcpy(dec_buf_ + dec_len_, data, copy);
            dec_len_ += copy;
        };
        decoder_.feed(enc, enc_len, out);
        decoder_.flush(out);

        // Return as many decoded bytes as requested.
        size_t n = max < dec_len_ ? max : dec_len_;
        memcpy(buf, dec_buf_, n);
        dec_pos_ = n;
        return n;
    }

private:
    ReadFn& inner_;
    uint32_t timeout_ms_;
    CobsDecoder decoder_;
    bool done_ = false;

    uint8_t dec_buf_[NOTE_COBS_BLOCK_SIZE + 1]{};
    size_t dec_pos_ = 0;
    size_t dec_len_ = 0;
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Streaming JSONB parser — reads opcodes from a byte source and dispatches
// SAX events through a SaxDispatch.
//
// The caller is responsible for COBS decoding and framing — this function
// receives raw JSONB opcodes (the payload between {: and :}).
//
// Uses SaxStreamBuf for read buffering and key/value accumulation.
// ---------------------------------------------------------------------------

namespace detail {

class JsonbParser {
public:
    JsonbParser(SaxStreamBuf& buf, SaxDispatch dispatch)
        : dispatch_(dispatch)
        , rbuf_(buf.rbuf), rbuf_cap_(buf.rbuf_size)
        , key_buf_(buf.key), key_cap_(buf.key_size)
        , val_buf_(buf.val), val_cap_(buf.val_size) {}

    template<typename ReadFn>
    string_view parse(ReadFn& read, uint32_t timeout_ms) {
        timeout_ms_ = timeout_ms;

        for (;;) {
            int opcode = read_byte(read);
            if (opcode < 0) break;

            switch (static_cast<uint8_t>(opcode)) {
            case jsonb::kBeginObject:
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_object_begin(current_key()));
                push_key();
                break;

            case jsonb::kEndObject:
                pop_key();
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_object_end(current_key()));
                break;

            case jsonb::kBeginArray:
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_array_begin(current_key()));
                break;

            case jsonb::kEndArray:
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_array_end(current_key()));
                break;

            case jsonb::kItem:
                key_len_ = read_string(read, key_buf_, key_cap_);
                break;

            case jsonb::kString: {
                size_t vlen = read_string(read, val_buf_, val_cap_);
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_string(current_key(),
                        string_view(val_buf_, vlen)));
                break;
            }

            // Signed integers — all widths dispatch as int32.
            case jsonb::kInt8: {
                uint8_t raw[1];
                if (!read_exact(read, raw, 1)) return NOTE_ERR("truncated int8");
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), static_cast<int8_t>(raw[0])));
                break;
            }
            case jsonb::kInt16: {
                uint8_t le[2];
                if (!read_exact(read, le, 2)) return NOTE_ERR("truncated int16");
                auto val = static_cast<int16_t>(uint16_t(le[0]) | (uint16_t(le[1]) << 8));
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), val));
                break;
            }
            case jsonb::kInt32: {
                uint8_t le[4];
                if (!read_exact(read, le, 4)) return NOTE_ERR("truncated int32");
                auto val = static_cast<int32_t>(
                    uint32_t(le[0]) | (uint32_t(le[1]) << 8) |
                    (uint32_t(le[2]) << 16) | (uint32_t(le[3]) << 24));
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), val));
                break;
            }
            case jsonb::kInt64: {
                uint8_t raw[8];
                if (!read_exact(read, raw, 8)) return NOTE_ERR("truncated int64");
                // Truncate to int32 — SaxEvent only carries int32.
                auto val = static_cast<int32_t>(
                    uint32_t(raw[0]) | (uint32_t(raw[1]) << 8) |
                    (uint32_t(raw[2]) << 16) | (uint32_t(raw[3]) << 24));
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), val));
                break;
            }

            // Unsigned integers — all widths dispatch as int32.
            case jsonb::kUint8: {
                uint8_t raw[1];
                if (!read_exact(read, raw, 1)) return NOTE_ERR("truncated uint8");
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), static_cast<int32_t>(raw[0])));
                break;
            }
            case jsonb::kUint16: {
                uint8_t le[2];
                if (!read_exact(read, le, 2)) return NOTE_ERR("truncated uint16");
                auto val = static_cast<int32_t>(uint16_t(le[0]) | (uint16_t(le[1]) << 8));
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), val));
                break;
            }
            case jsonb::kUint32: {
                uint8_t le[4];
                if (!read_exact(read, le, 4)) return NOTE_ERR("truncated uint32");
                auto val = static_cast<int32_t>(
                    uint32_t(le[0]) | (uint32_t(le[1]) << 8) |
                    (uint32_t(le[2]) << 16) | (uint32_t(le[3]) << 24));
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), val));
                break;
            }
            case jsonb::kUint64: {
                uint8_t raw[8];
                if (!read_exact(read, raw, 8)) return NOTE_ERR("truncated uint64");
                auto val = static_cast<int32_t>(
                    uint32_t(raw[0]) | (uint32_t(raw[1]) << 8) |
                    (uint32_t(raw[2]) << 16) | (uint32_t(raw[3]) << 24));
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_int(current_key(), val));
                break;
            }

            // Floats
            case jsonb::kFloat: {
                uint8_t raw[4];
                if (!read_exact(read, raw, 4)) return NOTE_ERR("truncated float");
                float fval;
                memcpy(&fval, raw, 4);
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_float(current_key(), static_cast<double>(fval)));
                break;
            }
            case jsonb::kDouble: {
                uint8_t raw[8];
                if (!read_exact(read, raw, 8)) return NOTE_ERR("truncated double");
                double val;
                memcpy(&val, raw, 8);
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_float(current_key(), val));
                break;
            }

            case jsonb::kTrue:
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_bool(current_key(), true));
                break;

            case jsonb::kFalse:
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_bool(current_key(), false));
                break;

            case jsonb::kNull:
                dispatch_.dispatch(dispatch_.sink,
                    SaxEvent::make_null(current_key()));
                break;

            default:
                return NOTE_ERR("unknown JSONB opcode");
            }
        }
        return {};
    }

private:
    SaxDispatch dispatch_;
    uint8_t* rbuf_;
    size_t rbuf_cap_;
    char* key_buf_;
    size_t key_cap_;
    char* val_buf_;
    size_t val_cap_;

    size_t rpos_ = 0;
    size_t rfill_ = 0;
    bool eof_ = false;
    uint32_t timeout_ms_ = 0;

    size_t key_len_ = 0;

    // Key stack for nested objects (same approach as SaxAdapter).
    static constexpr uint8_t kMaxDepth = 8;
    static constexpr uint8_t kMaxKeyPerLevel = 32;
    struct KeySlot {
        char saved[kMaxKeyPerLevel]{};
        uint8_t length = 0;
    };
    KeySlot key_stack_[kMaxDepth]{};
    uint8_t stack_depth_ = 0;

    string_view current_key() const {
        return string_view(key_buf_, key_len_);
    }

    void push_key() {
        if (stack_depth_ < kMaxDepth) {
            auto save_len = static_cast<uint8_t>(
                key_len_ < kMaxKeyPerLevel ? key_len_ : kMaxKeyPerLevel);
            for (uint8_t i = 0; i < save_len; ++i)
                key_stack_[stack_depth_].saved[i] = key_buf_[i];
            key_stack_[stack_depth_].length = save_len;
            ++stack_depth_;
        }
        key_len_ = 0;
    }

    void pop_key() {
        if (stack_depth_ > 0) {
            --stack_depth_;
            auto restore_len = key_stack_[stack_depth_].length;
            for (uint8_t i = 0; i < restore_len && i < key_cap_; ++i)
                key_buf_[i] = key_stack_[stack_depth_].saved[i];
            key_len_ = restore_len < key_cap_ ? restore_len : key_cap_;
        } else {
            key_len_ = 0;
        }
    }

    template<typename ReadFn>
    int read_byte(ReadFn& read) {
        if (rpos_ >= rfill_) {
            if (eof_) return -1;
            auto r = read(rbuf_, rbuf_cap_, timeout_ms_);
            if (!r || *r == 0) { eof_ = true; return -1; }
            rfill_ = *r;
            rpos_ = 0;
        }
        return rbuf_[rpos_++];
    }

    template<typename ReadFn>
    bool read_exact(ReadFn& read, uint8_t* dst, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            int b = read_byte(read);
            if (b < 0) return false;
            dst[i] = static_cast<uint8_t>(b);
        }
        return true;
    }

    template<typename ReadFn>
    size_t read_string(ReadFn& read, char* dst, size_t cap) {
        size_t len = 0;
        for (;;) {
            int c = read_byte(read);
            if (c <= 0) break;  // EOF or null terminator
            if (len < cap) dst[len] = static_cast<char>(c);
            ++len;
        }
        return len < cap ? len : cap;
    }
};

}  // namespace detail

/// Parse a JSONB opcode stream from a streaming byte source.
/// ReadFn signature: Result<size_t>(uint8_t* buf, size_t max, uint32_t timeout_ms)
template<typename ReadFn>
string_view jsonb_parse_streaming(ReadFn&& read, uint32_t timeout_ms,
                                   SaxStreamBuf& buf, SaxDispatch dispatch) {
    detail::JsonbParser parser(buf, dispatch);
    return parser.parse(read, timeout_ms);
}

/// Convenience overload with default stack buffers.
template<typename ReadFn>
string_view jsonb_parse_streaming(ReadFn&& read, uint32_t timeout_ms,
                                   SaxDispatch dispatch) {
    char storage[384];
    SaxStreamBuf buf(storage);
    return jsonb_parse_streaming(std::forward<ReadFn>(read), timeout_ms, buf, dispatch);
}

}  // namespace note
