// Streaming SAX parser — incremental parsing from a byte source.
//
// Same JsonSink interface as json_sax.hpp, but reads from a transport
// read function instead of a complete buffer. Characters are pulled
// on demand via peek()/advance(), which block on the read function
// when the small internal buffer is exhausted.
//
// All strings (keys and values) are accumulated into scratch buffers,
// since the read buffer is a small sliding window that can't be referenced
// after refill. Buffers can be caller-provided for full memory control,
// or left to defaults (384 bytes on the stack).
//
// Usage (default buffers):
//   auto err = note::sax_parse_streaming(read_fn, 5000, sink);
//
// Usage (caller-provided buffer):
//   char buf[512];
//   note::SaxStreamBuf sbuf(buf);
//   auto err = note::sax_parse_streaming(read_fn, 5000, sbuf, sink);
#pragma once

#include "json_sax.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>

namespace note {

/// Buffer layout for the streaming SAX parser.
///
/// Partitions a caller-provided buffer into three regions:
///   [read buf | key scratch | value scratch]
///
/// Sizing: the read buffer gets 1/6, key scratch 1/6, value scratch 4/6.
/// Minimum useful size is ~48 bytes (8 read + 8 key + 32 value).
struct SaxStreamBuf {
    uint8_t* rbuf;       ///< Transport read buffer
    size_t rbuf_size;
    char* key;           ///< Key accumulation scratch
    size_t key_size;
    char* val;           ///< Value accumulation scratch
    size_t val_size;

    /// Partition a single buffer automatically.
    template<size_t N>
    explicit SaxStreamBuf(char (&buf)[N])
        : SaxStreamBuf(buf, N) {}

    SaxStreamBuf(char* buf, size_t len) {
        size_t sixth = len / 6;
        if (sixth < 8) sixth = 8;
        rbuf = reinterpret_cast<uint8_t*>(buf);
        rbuf_size = sixth;
        key = buf + sixth;
        key_size = sixth;
        val = buf + 2 * sixth;
        val_size = len - 2 * sixth;
    }

    /// Explicit region sizes for full control.
    SaxStreamBuf(uint8_t* rb, size_t rbs, char* k, size_t ks, char* v, size_t vs)
        : rbuf(rb), rbuf_size(rbs), key(k), key_size(ks), val(v), val_size(vs) {}
};

namespace detail {

/// Streaming SAX parser that pulls bytes from a read function.
///
/// ReadFn must be callable as: Result<size_t>(uint8_t* buf, size_t max, uint32_t timeout_ms)
template<typename ReadFn, typename SinkT = JsonSink>
class StreamingSaxParser {
public:
    StreamingSaxParser(ReadFn& read, uint32_t timeout_ms,
                       SaxStreamBuf& buf, SinkT& sink)
        : read_(read), timeout_ms_(timeout_ms), buf_(buf), sink_(sink) {}

    string_view parse() {
        skip_ws();
        if (at_end()) return "empty input";
        if (peek() != '{') return NOTE_ERR("expected object");
        auto err = parse_object({});
        if (!err.empty()) return err;
        return {};
    }

private:
    ReadFn& read_;
    uint32_t timeout_ms_;
    SaxStreamBuf& buf_;
    SinkT& sink_;

    // Read state — indexes into buf_.rbuf
    size_t rpos_ = 0;
    size_t rfill_ = 0;
    bool eof_ = false;

    // ── Byte source ──────────────────────────────────────────────────────

    void refill() {
        auto r = read_(buf_.rbuf, buf_.rbuf_size, timeout_ms_);
        if (!r || *r == 0) {
            eof_ = true;
            rfill_ = 0;
        } else {
            rfill_ = *r;
        }
        rpos_ = 0;
    }

    char peek() {
        if (rpos_ >= rfill_ && !eof_) refill();
        return rpos_ < rfill_ ? static_cast<char>(buf_.rbuf[rpos_]) : '\0';
    }

    char advance() {
        if (rpos_ >= rfill_ && !eof_) refill();
        return rpos_ < rfill_ ? static_cast<char>(buf_.rbuf[rpos_++]) : '\0';
    }

    bool at_end() {
        if (rpos_ < rfill_) return false;
        if (!eof_) refill();
        return rpos_ >= rfill_;
    }

    // ── Whitespace ───────────────────────────────────────────────────────

    void skip_ws() {
        while (!at_end()) {
            char c = peek();
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            advance();
        }
    }

    // ── String parsing ───────────────────────────────────────────────────

    string_view parse_string_into(char* scratch, size_t scratch_size,
                                   string_view& result) {
        if (advance() != '"') return "expected '\"'";
        size_t out = 0;

        while (!at_end()) {
            char c = peek();
            if (c == '"') {
                advance();
                result = {scratch, out};
                return {};
            }
            if (c == '\\') {
                advance();
                auto err = parse_escape(scratch, scratch_size, out);
                if (!err.empty()) return err;
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t')
                return NOTE_ERR("unescaped control character in string");
            advance();
            if (out < scratch_size) scratch[out++] = c;
        }
        return NOTE_ERR("unterminated string");
    }

    string_view parse_escape(char* scratch, size_t scratch_size, size_t& out) {
        if (at_end()) return NOTE_ERR("unexpected end in escape");
        char esc = advance();
        switch (esc) {
        case '"':  if (out < scratch_size) scratch[out++] = '"';  return {};
        case '\\': if (out < scratch_size) scratch[out++] = '\\'; return {};
        case '/':  if (out < scratch_size) scratch[out++] = '/';  return {};
        case 'b':  if (out < scratch_size) scratch[out++] = '\b'; return {};
        case 'f':  if (out < scratch_size) scratch[out++] = '\f'; return {};
        case 'n':  if (out < scratch_size) scratch[out++] = '\n'; return {};
        case 'r':  if (out < scratch_size) scratch[out++] = '\r'; return {};
        case 't':  if (out < scratch_size) scratch[out++] = '\t'; return {};
        case 'u': {
            uint16_t cp = 0;
            for (int i = 0; i < 4; ++i) {
                if (at_end()) return NOTE_ERR("incomplete \\u escape");
                char h = advance();
                cp = static_cast<uint16_t>(cp << 4);
                if (h >= '0' && h <= '9') cp = static_cast<uint16_t>(cp | (h - '0'));
                else if (h >= 'a' && h <= 'f') cp = static_cast<uint16_t>(cp | (h - 'a' + 10));
                else if (h >= 'A' && h <= 'F') cp = static_cast<uint16_t>(cp | (h - 'A' + 10));
                else return NOTE_ERR("invalid hex in \\u escape");
            }
            if (cp < 0x80) {
                if (out < scratch_size) scratch[out++] = static_cast<char>(cp);
            } else if (cp < 0x800) {
                if (out + 1 < scratch_size) {
                    scratch[out++] = static_cast<char>(0xC0 | (cp >> 6));
                    scratch[out++] = static_cast<char>(0x80 | (cp & 0x3F));
                }
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                if (out < scratch_size) scratch[out++] = '?';
            } else {
                if (out + 2 < scratch_size) {
                    scratch[out++] = static_cast<char>(0xE0 | (cp >> 12));
                    scratch[out++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    scratch[out++] = static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
            return {};
        }
        default:
            if (out < scratch_size) scratch[out++] = esc;
            return {};
        }
    }

    // ── Value parsing ────────────────────────────────────────────────────

    string_view parse_object(string_view parent_key) {
        advance();
        sink_.on_object_begin(parent_key);
        skip_ws();

        if (peek() == '}') {
            advance();
            sink_.on_object_end(parent_key);
            return {};
        }

        while (true) {
            skip_ws();
            string_view key;
            auto err = parse_string_into(buf_.key, buf_.key_size, key);
            if (!err.empty()) return err;

            skip_ws();
            if (advance() != ':') return NOTE_ERR("expected ':'");
            skip_ws();

            err = parse_value(key);
            if (!err.empty()) return err;

            skip_ws();
            char c = advance();
            if (c == '}') {
                sink_.on_object_end(parent_key);
                return {};
            }
            if (c != ',') return NOTE_ERR("expected ',' or '}'");
        }
    }

    string_view parse_array(string_view key) {
        advance();
        sink_.on_array_begin(key);
        skip_ws();

        if (peek() == ']') {
            advance();
            sink_.on_array_end(key);
            return {};
        }

        while (true) {
            skip_ws();
            auto err = parse_value(key);
            if (!err.empty()) return err;

            skip_ws();
            char c = advance();
            if (c == ']') {
                sink_.on_array_end(key);
                return {};
            }
            if (c != ',') return NOTE_ERR("expected ',' or ']'");
        }
    }

    string_view parse_value(string_view key) {
        char c = peek();

        if (c == '"') {
            string_view val;
            auto err = parse_string_into(buf_.val, buf_.val_size, val);
            if (!err.empty()) return err;
            sink_.on_string(key, val);
            return {};
        }

        if (c == '{') return parse_object(key);
        if (c == '[') return parse_array(key);

        if (c == 't') return parse_literal("true", 4, key, true);
        if (c == 'f') return parse_literal("false", 5, key, false);
        if (c == 'n') return parse_null(key);

        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number(key);

        return NOTE_ERR("unexpected character");
    }

    string_view parse_literal(const char* lit, size_t lit_len,
                               string_view key, bool value) {
        for (size_t i = 0; i < lit_len; ++i) {
            if (at_end()) return NOTE_ERR("unexpected end");
            if (advance() != lit[i]) return NOTE_ERR("invalid literal");
        }
        sink_.on_bool(key, value);
        return {};
    }

    string_view parse_null(string_view key) {
        const char* lit = "null";
        for (size_t i = 0; i < 4; ++i) {
            if (at_end()) return NOTE_ERR("unexpected end");
            if (advance() != lit[i]) return NOTE_ERR("invalid literal");
        }
        sink_.on_null(key);
        return {};
    }

    string_view parse_number(string_view key) {
        size_t out = 0;

        if (peek() == '-') {
            if (out < buf_.val_size) buf_.val[out++] = advance();
            else advance();
        }

        if (at_end()) return NOTE_ERR("unexpected end in number");

        if (peek() == '0') {
            if (out < buf_.val_size) buf_.val[out++] = advance();
            else advance();
        } else if (peek() >= '1' && peek() <= '9') {
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                if (out < buf_.val_size) buf_.val[out++] = advance();
                else advance();
            }
        } else {
            return NOTE_ERR("invalid number");
        }

        if (!at_end() && peek() == '.') {
            if (out < buf_.val_size) buf_.val[out++] = advance();
            else advance();
            if (at_end() || peek() < '0' || peek() > '9')
                return NOTE_ERR("invalid number: digit expected after '.'");
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                if (out < buf_.val_size) buf_.val[out++] = advance();
                else advance();
            }
        }

        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            if (out < buf_.val_size) buf_.val[out++] = advance();
            else advance();
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                if (out < buf_.val_size) buf_.val[out++] = advance();
                else advance();
            }
            if (at_end() || peek() < '0' || peek() > '9')
                return NOTE_ERR("invalid number: digit expected in exponent");
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                if (out < buf_.val_size) buf_.val[out++] = advance();
                else advance();
            }
        }

        sink_.on_number(key, {buf_.val, out});
        return {};
    }
};

}  // namespace detail

/// Parse JSON from a streaming byte source with caller-provided buffers.
template<typename ReadFn, typename SinkT = JsonSink>
string_view sax_parse_streaming(ReadFn&& read, uint32_t timeout_ms,
                                 SaxStreamBuf& buf, SinkT& sink) {
    detail::StreamingSaxParser<std::remove_reference_t<ReadFn>, SinkT> parser(read, timeout_ms, buf, sink);
    return parser.parse();
}

/// Parse JSON from a streaming byte source with default stack buffers.
template<typename ReadFn, typename SinkT = JsonSink>
string_view sax_parse_streaming(ReadFn&& read, uint32_t timeout_ms, SinkT& sink) {
    char storage[384];  // 64 read + 64 key + 256 value
    SaxStreamBuf buf(storage);
    return sax_parse_streaming(std::forward<ReadFn>(read), timeout_ms, buf, sink);
}

}  // namespace note
