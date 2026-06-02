// SAX-style JSON parser — true streaming, zero allocation.
//
// Parses a JSON string character-by-character and fires callbacks on a
// JsonSink as values are encountered. No token buffer, no tree, no heap.
//
// The parser handles the full JSON grammar (RFC 8259) for the types that
// Notecard produces: objects, strings, numbers, booleans, null, and arrays.
//
// Usage:
//   struct MySink : note::JsonSink { ... };
//   MySink sink;
//   auto err = note::sax_parse(json, len, sink);
#pragma once

#include "string_pool.hpp"
#include "types.hpp"
#include "detail/number_parse.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace note {

namespace detail {
    inline size_t format_int(char* buf, json_int_t v) {
        // Simple int-to-string without snprintf dependency
        if (v == 0) { buf[0] = '0'; return 1; }
        size_t pos = 0;
        bool neg = v < 0;
        uint64_t abs = neg ? static_cast<uint64_t>(-(v + 1)) + 1u : static_cast<uint64_t>(v);
        char tmp[20];
        size_t tpos = 0;
        while (abs > 0) { tmp[tpos++] = static_cast<char>('0' + abs % 10); abs /= 10; }
        if (neg) buf[pos++] = '-';
        for (size_t i = tpos; i > 0; --i) buf[pos++] = tmp[i - 1];
        return pos;
    }
    inline size_t format_float(char* buf, double v) {
        // Use snprintf for doubles — no simple way to avoid it
        int n = snprintf(buf, 24, "%.17g", v);
        return n > 0 ? static_cast<size_t>(n) : 0;
    }
} // namespace detail

// ---------------------------------------------------------------------------
// SinkLike — compile-time check that a type satisfies the sink interface.
// Used by ErrorCaptureSinkT, FilterSink, and StaticNotecard's NullSink.
// ---------------------------------------------------------------------------
#if __cplusplus >= 202002L
template<typename T>
concept SinkLike = requires(T t, string_view k, string_view v, bool b,
                            json_int_t i, double f) {
    t.on_null(k);
    t.on_bool(k, b);
    t.on_int(k, i);
    t.on_float(k, f);
    t.on_number(k, v);
    t.on_string(k, v);
    t.on_object_begin(k);
    t.on_object_end(k);
    t.on_array_begin(k);
    t.on_array_end(k);
    t.reset();
};
#endif

// ---------------------------------------------------------------------------
// JsonSink — callback interface driven by the SAX parser.
//
// The parser calls these methods as it encounters JSON values. Object keys
// are delivered via on_key(), followed by the corresponding value callback.
// Implementations typically dispatch on the most recent key.
// ---------------------------------------------------------------------------
class JsonSink {
public:
    virtual ~JsonSink() = default;

    virtual void on_null(string_view key) { (void)key; }
    virtual void on_bool(string_view key, bool value) { (void)key; (void)value; }
    virtual void on_number(string_view key, string_view raw) { (void)key; (void)raw; }
    virtual void on_int(string_view key, json_int_t value) {
        char buf[24];
        auto n = detail::format_int(buf, value);
        on_number(key, string_view(buf, n));
    }
    virtual void on_float(string_view key, double value) {
        char buf[24];
        auto n = detail::format_float(buf, value);
        on_number(key, string_view(buf, n));
    }
    virtual void on_string(string_view key, string_view value) { (void)key; (void)value; }

    // Called when a nested object begins/ends. The parser delivers the object's
    // key-value pairs between begin/end. For flat objects, these are only
    // called for the root.
    virtual void on_object_begin(string_view key) { (void)key; }
    virtual void on_object_end(string_view key) { (void)key; }

    // Called when an array begins/ends.
    virtual void on_array_begin(string_view key) { (void)key; }
    virtual void on_array_end(string_view key) { (void)key; }

    virtual void reset() {}
};

// ---------------------------------------------------------------------------
// FilterJsonSink — forwarding base class for sink filters.
//
// All callbacks forward to an inner sink. Subclasses override only what
// they intercept. Used by ErrorCaptureSink, CrcFieldSink, etc.
// ---------------------------------------------------------------------------
class FilterJsonSink : public JsonSink {
protected:
    JsonSink& inner_;
public:
    explicit FilterJsonSink(JsonSink& inner) : inner_(inner) {}

    void on_null(string_view key) override { inner_.on_null(key); }
    void on_bool(string_view key, bool value) override { inner_.on_bool(key, value); }
    void on_number(string_view key, string_view raw) override { inner_.on_number(key, raw); }
    void on_int(string_view key, json_int_t value) override { inner_.on_int(key, value); }
    void on_float(string_view key, double value) override { inner_.on_float(key, value); }
    void on_string(string_view key, string_view value) override { inner_.on_string(key, value); }
    void on_object_begin(string_view key) override { inner_.on_object_begin(key); }
    void on_object_end(string_view key) override { inner_.on_object_end(key); }
    void on_array_begin(string_view key) override { inner_.on_array_begin(key); }
    void on_array_end(string_view key) override { inner_.on_array_end(key); }
    void reset() override { inner_.reset(); }
};

// ---------------------------------------------------------------------------
// TeeSink — forwards each SAX event to a pair of inner sinks.
//
// Pair-shape so the composition is explicit; chain three or more sinks by
// nesting TeeSinks (TeeSink t1{a, b}; TeeSink t2{t1, c};). Used by tree
// presentation to fan SAX events out to the backend's tree assembler +
// optional body handler + optional debug-receive capture.
// ---------------------------------------------------------------------------
class TeeSink : public JsonSink {
public:
    TeeSink(JsonSink& a, JsonSink& b) : a_(a), b_(b) {}

    void on_null  (string_view k) override                { a_.on_null(k);          b_.on_null(k); }
    void on_bool  (string_view k, bool v) override        { a_.on_bool(k, v);       b_.on_bool(k, v); }
    void on_int   (string_view k, json_int_t v) override  { a_.on_int(k, v);        b_.on_int(k, v); }
    void on_float (string_view k, double v) override      { a_.on_float(k, v);      b_.on_float(k, v); }
    void on_number(string_view k, string_view r) override { a_.on_number(k, r);     b_.on_number(k, r); }
    void on_string(string_view k, string_view v) override { a_.on_string(k, v);     b_.on_string(k, v); }
    void on_object_begin(string_view k) override          { a_.on_object_begin(k);  b_.on_object_begin(k); }
    void on_object_end  (string_view k) override          { a_.on_object_end(k);    b_.on_object_end(k); }
    void on_array_begin (string_view k) override          { a_.on_array_begin(k);   b_.on_array_begin(k); }
    void on_array_end   (string_view k) override          { a_.on_array_end(k);     b_.on_array_end(k); }
    void reset() override                                 { a_.reset();             b_.reset(); }

private:
    JsonSink& a_;
    JsonSink& b_;
};

// ---------------------------------------------------------------------------
// Template filter/capture sinks — zero vtable overhead.
// Used by the static (non-virtual) execute path.
// ---------------------------------------------------------------------------

/// Non-virtual base with all sink methods as no-ops. Use as a starting
/// point when writing sinks that only handle a subset of events.
struct NullSink {
    void on_null(string_view) {}
    void on_bool(string_view, bool) {}
    void on_int(string_view, json_int_t) {}
    void on_float(string_view, double) {}
    void on_number(string_view, string_view) {}
    void on_string(string_view, string_view) {}
    void on_object_begin(string_view) {}
    void on_object_end(string_view) {}
    void on_array_begin(string_view) {}
    void on_array_end(string_view) {}
    void reset() {}
};
#if __cplusplus >= 202002L
static_assert(SinkLike<NullSink>, "NullSink must satisfy SinkLike");
#endif

/// Filter sink that forwards to a concrete inner sink (no vtable).
template<typename InnerT>
#if __cplusplus >= 202002L
    requires SinkLike<InnerT>
#endif
class FilterSink {
protected:
    InnerT& inner_;
public:
    explicit FilterSink(InnerT& inner) : inner_(inner) {}

    void on_null(string_view key) { inner_.on_null(key); }
    void on_bool(string_view key, bool value) { inner_.on_bool(key, value); }
    void on_number(string_view key, string_view raw) { inner_.on_number(key, raw); }
    void on_int(string_view key, json_int_t value) { inner_.on_int(key, value); }
    void on_float(string_view key, double value) { inner_.on_float(key, value); }
    void on_string(string_view key, string_view value) { inner_.on_string(key, value); }
    void on_object_begin(string_view key) { inner_.on_object_begin(key); }
    void on_object_end(string_view key) { inner_.on_object_end(key); }
    void on_array_begin(string_view key) { inner_.on_array_begin(key); }
    void on_array_end(string_view key) { inner_.on_array_end(key); }
    void reset() { inner_.reset(); }
};

/// Error capture sink that wraps a concrete inner sink (no vtable).
/// Copies the error string into a local buffer (same reason as ErrorCaptureSink).
template<typename InnerT>
class ErrorCaptureSinkT : public FilterSink<InnerT> {
    static constexpr size_t kMaxErrLen = 64;
    char err_buf_[kMaxErrLen];
    string_view err_;
public:
    explicit ErrorCaptureSinkT(InnerT& inner) : FilterSink<InnerT>(inner) {}

    string_view captured_error() const { return err_; }

    void on_string(string_view key, string_view value) {
        if (key == "err") {
            size_t len = value.size() < kMaxErrLen ? value.size() : kMaxErrLen;
            for (size_t i = 0; i < len; ++i) err_buf_[i] = value[i];
            err_ = string_view(err_buf_, len);
            return;
        }
        this->inner_.on_string(key, value);
    }

    void reset() {
        err_ = {};
        FilterSink<InnerT>::reset();
    }
};

/// Non-virtual base with default no-op implementations.
/// Generated Response::Sink types inherit this instead of JsonSink,
/// keeping them vtable-free while providing default stubs.
struct DefaultSink {
    void on_null(string_view) {}
    void on_bool(string_view, bool) {}
    void on_number(string_view, string_view) {}
    void on_int(string_view, json_int_t) {}
    void on_float(string_view, double) {}
    void on_string(string_view, string_view) {}
    void on_object_begin(string_view) {}
    void on_object_end(string_view) {}
    void on_array_begin(string_view) {}
    void on_array_end(string_view) {}
    void reset() {}
};

/// Adapter that wraps any concrete sink type into a virtual JsonSink.
/// Used by the virtual Notecard path to bridge non-virtual Response::Sink
/// types into the virtual ErrorCaptureSink/FilterJsonSink chain.
template<typename T>
class JsonSinkAdapter : public JsonSink {
    T& inner_;
public:
    explicit JsonSinkAdapter(T& inner) : inner_(inner) {}
    void on_null(string_view key) override { inner_.on_null(key); }
    void on_bool(string_view key, bool value) override { inner_.on_bool(key, value); }
    void on_number(string_view key, string_view raw) override { inner_.on_number(key, raw); }
    void on_int(string_view key, json_int_t value) override { inner_.on_int(key, value); }
    void on_float(string_view key, double value) override { inner_.on_float(key, value); }
    void on_string(string_view key, string_view value) override { inner_.on_string(key, value); }
    void on_object_begin(string_view key) override { inner_.on_object_begin(key); }
    void on_object_end(string_view key) override { inner_.on_object_end(key); }
    void on_array_begin(string_view key) override { inner_.on_array_begin(key); }
    void on_array_end(string_view key) override { inner_.on_array_end(key); }
    void reset() override { inner_.reset(); }
};

// ---------------------------------------------------------------------------
// SAX parser — zero allocation, single pass, streaming.
//
// Returns empty string_view on success, or an error message on parse failure.
// The sink receives callbacks as the JSON is scanned.
// ---------------------------------------------------------------------------

namespace detail {

// Parser state machine
class SaxParser {
public:
    SaxParser(const char* json, size_t len, JsonSink& sink)
        : json_(json), len_(len), sink_(sink) {}

    string_view parse() {
        skip_ws();
        if (pos_ >= len_) return "empty input";

        if (json_[pos_] != '{') return NOTE_ERR("expected object");
        auto err = parse_object({});
        if (!err.empty()) return err;

        skip_ws();
        if (pos_ < len_) return "trailing content";
        return {};
    }

private:
    const char* json_;
    size_t len_;
    size_t pos_ = 0;
    JsonSink& sink_;

    // Scratch buffer for unescaping strings. Sized for the longest string
    // value we expect from Notecard responses. Strings longer than this
    // are truncated in the unescaped form.
    static constexpr size_t kScratchSize = 256;
    char scratch_[kScratchSize];

    char peek() const { return pos_ < len_ ? json_[pos_] : '\0'; }
    char advance() { return pos_ < len_ ? json_[pos_++] : '\0'; }

    static bool is_ws(unsigned char c) {
        switch (c) {
        case ' ': case '\t': case '\n': case '\r': return true;
        default: return false;
        }
    }

    static bool is_digit(char c) {
        return static_cast<unsigned>(c - '0') < 10;
    }

    static bool is_hex(char c) {
        return is_digit(c)
            || (static_cast<unsigned>(c - 'a') < 6)
            || (static_cast<unsigned>(c - 'A') < 6);
    }

    /// Returns hex digit value (0-15) or -1 if not hex.
    static int hex_val(char c) {
        if (is_digit(c)) return c - '0';
        if (static_cast<unsigned>(c - 'a') < 6) return c - 'a' + 10;
        if (static_cast<unsigned>(c - 'A') < 6) return c - 'A' + 10;
        return -1;
    }

    void skip_ws() {
        while (pos_ < len_ && is_ws(static_cast<unsigned char>(json_[pos_])))
            ++pos_;
    }

    // Parse a JSON string. Returns the raw content between quotes.
    // If the string contains escape sequences, it's unescaped into scratch_.
    // Sets `view` to point to either the original JSON or scratch_.
    string_view parse_string_err(string_view& view) {
        if (advance() != '"') return "expected '\"'";
        size_t start = pos_;
        bool has_escape = false;

        // Fast scan for end of string
        while (pos_ < len_) {
            char c = json_[pos_];
            if (c == '"') {
                if (!has_escape) {
                    view = {json_ + start, pos_ - start};
                } else {
                    view = unescape(json_ + start, pos_ - start);
                }
                ++pos_;  // skip closing quote
                return {};
            }
            if (c == '\\') {
                has_escape = true;
                ++pos_;  // skip backslash
                if (pos_ >= len_) return NOTE_ERR("unexpected end in escape");
                char esc = json_[pos_];
                if (esc == 'u') {
                    // Skip \uXXXX
                    ++pos_;
                    for (int i = 0; i < 4 && pos_ < len_; ++i, ++pos_) {
                        char h = json_[pos_];
                        if (!is_hex(h))
                            return NOTE_ERR("invalid hex in \\u escape");
                    }
                    continue;
                }
                // Other escapes: just skip the escaped char
            }
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t')
                return NOTE_ERR("unescaped control character in string");
            ++pos_;
        }
        return NOTE_ERR("unterminated string");
    }

    // Unescape a JSON string into scratch buffer.
    string_view unescape(const char* src, size_t src_len) {
        size_t out = 0;
        size_t i = 0;
        while (i < src_len && out < kScratchSize - 1) {
            if (src[i] == '\\' && i + 1 < src_len) {
                ++i;
                switch (src[i]) {
                case '"':  scratch_[out++] = '"'; break;
                case '\\': scratch_[out++] = '\\'; break;
                case '/':  scratch_[out++] = '/'; break;
                case 'b':  scratch_[out++] = '\b'; break;
                case 'f':  scratch_[out++] = '\f'; break;
                case 'n':  scratch_[out++] = '\n'; break;
                case 'r':  scratch_[out++] = '\r'; break;
                case 't':  scratch_[out++] = '\t'; break;
                case 'u': {
                    // Decode \uXXXX — for BMP characters, emit UTF-8.
                    // Surrogate pairs are simplified to '?' for now.
                    ++i;
                    uint16_t cp = 0;
                    for (int j = 0; j < 4 && i < src_len; ++j, ++i) {
                        int hv = hex_val(src[i]);
                        if (hv >= 0) cp = static_cast<uint16_t>((cp << 4) | hv);
                    }
                    if (cp < 0x80) {
                        scratch_[out++] = static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        if (out + 1 < kScratchSize) {
                            scratch_[out++] = static_cast<char>(0xC0 | (cp >> 6));
                            scratch_[out++] = static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    } else {
                        // BMP or surrogate — emit UTF-8 for BMP
                        if (cp >= 0xD800 && cp <= 0xDFFF) {
                            scratch_[out++] = '?';  // surrogate placeholder
                        } else if (out + 2 < kScratchSize) {
                            scratch_[out++] = static_cast<char>(0xE0 | (cp >> 12));
                            scratch_[out++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            scratch_[out++] = static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    }
                    continue;  // i already advanced past the 4 hex digits
                }
                default:
                    scratch_[out++] = src[i];
                    break;
                }
                ++i;
            } else {
                scratch_[out++] = src[i++];
            }
        }
        return {scratch_, out};
    }

    // Parse a JSON object. `key` is the key of this object in its parent
    // (empty for root).
    string_view parse_object(string_view key) {
        ++pos_;  // skip '{'
        sink_.on_object_begin(key);
        skip_ws();

        if (peek() == '}') {
            ++pos_;
            sink_.on_object_end(key);
            return {};
        }

        while (true) {
            skip_ws();
            // Parse key
            string_view k;
            auto err = parse_string_err(k);
            if (!err.empty()) return err;

            skip_ws();
            if (advance() != ':') return NOTE_ERR("expected ':'");
            skip_ws();

            // Parse value
            err = parse_value(k);
            if (!err.empty()) return err;

            skip_ws();
            char c = advance();
            if (c == '}') {
                sink_.on_object_end(key);
                return {};
            }
            if (c != ',') return NOTE_ERR("expected ',' or '}'");
        }
    }

    // Parse a JSON array.
    string_view parse_array(string_view key) {
        ++pos_;  // skip '['
        sink_.on_array_begin(key);
        skip_ws();

        if (peek() == ']') {
            ++pos_;
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

    // Parse any JSON value and fire the appropriate sink callback.
    string_view parse_value(string_view key) {
        char c = peek();

        if (c == '"') {
            string_view val;
            auto err = parse_string_err(val);
            if (!err.empty()) return err;
            sink_.on_string(key, val);
            return {};
        }

        if (c == '{') return parse_object(key);
        if (c == '[') return parse_array(key);

        if (c == 't') return parse_literal("true", 4, key, true);
        if (c == 'f') return parse_literal("false", 5, key, false);
        if (c == 'n') return parse_null(key);

        if (c == '-' || is_digit(c))
            return parse_number(key);

        return NOTE_ERR("unexpected character");
    }

    string_view parse_literal(const char* lit, size_t lit_len,
                               string_view key, bool value) {
        if (pos_ + lit_len > len_) return NOTE_ERR("unexpected end");
        for (size_t i = 0; i < lit_len; ++i) {
            if (json_[pos_ + i] != lit[i]) return NOTE_ERR("invalid literal");
        }
        pos_ += lit_len;
        sink_.on_bool(key, value);
        return {};
    }

    string_view parse_null(string_view key) {
        if (pos_ + 4 > len_) return NOTE_ERR("unexpected end");
        if (json_[pos_] != 'n' || json_[pos_+1] != 'u' ||
            json_[pos_+2] != 'l' || json_[pos_+3] != 'l')
            return NOTE_ERR("invalid literal");
        pos_ += 4;
        sink_.on_null(key);
        return {};
    }

    string_view parse_number(string_view key) {
        size_t start = pos_;
        if (peek() == '-') ++pos_;

        if (pos_ >= len_) return NOTE_ERR("unexpected end in number");

        // Integer part
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (pos_ < len_ && json_[pos_] >= '0' && json_[pos_] <= '9')
                ++pos_;
        } else {
            return NOTE_ERR("invalid number");
        }

        // Fractional part
        if (pos_ < len_ && json_[pos_] == '.') {
            ++pos_;
            if (pos_ >= len_ || json_[pos_] < '0' || json_[pos_] > '9')
                return NOTE_ERR("invalid number: digit expected after '.'");
            while (pos_ < len_ && json_[pos_] >= '0' && json_[pos_] <= '9')
                ++pos_;
        }

        // Exponent
        if (pos_ < len_ && (json_[pos_] == 'e' || json_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < len_ && (json_[pos_] == '+' || json_[pos_] == '-'))
                ++pos_;
            if (pos_ >= len_ || json_[pos_] < '0' || json_[pos_] > '9')
                return NOTE_ERR("invalid number: digit expected in exponent");
            while (pos_ < len_ && json_[pos_] >= '0' && json_[pos_] <= '9')
                ++pos_;
        }

        sink_.on_number(key, {json_ + start, pos_ - start});
        return {};
    }
};

}  // namespace detail

// Parse a JSON object and deliver events to sink.
// Returns empty string_view on success, error message on failure.
inline string_view sax_parse(const char* json, size_t len, JsonSink& sink) {
    detail::SaxParser parser(json, len, sink);
    return parser.parse();
}

inline string_view sax_parse(string_view json, JsonSink& sink) {
    return sax_parse(json.data(), json.size(), sink);
}

// ---------------------------------------------------------------------------
// ErrorCaptureSink — wraps any JsonSink, intercepts the "err" key.
//
// Used by the SAX execute path to detect Notecard errors without a separate
// get_reader() pre-pass. All non-error events are forwarded to the inner sink.
//
// Copies the error string into a local buffer so it outlives the SAX parser's
// scratch buffers (which are destroyed when sax_parse_streaming returns).
// ---------------------------------------------------------------------------
class ErrorCaptureSink : public FilterJsonSink {
    static constexpr size_t kMaxErrLen = 64;
    char err_buf_[kMaxErrLen];
    string_view err_;
public:
    explicit ErrorCaptureSink(JsonSink& inner) : FilterJsonSink(inner) {}

    string_view captured_error() const { return err_; }

    void on_string(string_view key, string_view value) override {
        if (key == "err") {
            size_t len = value.size() < kMaxErrLen ? value.size() : kMaxErrLen;
            for (size_t i = 0; i < len; ++i) err_buf_[i] = value[i];
            err_ = string_view(err_buf_, len);
            return;
        }
        inner_.on_string(key, value);
    }

    void reset() override {
        err_ = {};
        FilterJsonSink::reset();
    }
};

}  // namespace note
