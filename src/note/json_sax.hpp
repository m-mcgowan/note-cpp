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

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace note {

namespace detail {
    inline size_t format_int(char* buf, int32_t v) {
        // Simple int-to-string without snprintf dependency
        if (v == 0) { buf[0] = '0'; return 1; }
        size_t pos = 0;
        bool neg = v < 0;
        uint32_t abs = neg ? static_cast<uint32_t>(-(v + 1)) + 1u : static_cast<uint32_t>(v);
        char tmp[11];
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
    virtual void on_int(string_view key, int32_t value) {
        char buf[12];
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
    void on_int(string_view key, int32_t value) override { inner_.on_int(key, value); }
    void on_float(string_view key, double value) override { inner_.on_float(key, value); }
    void on_string(string_view key, string_view value) override { inner_.on_string(key, value); }
    void on_object_begin(string_view key) override { inner_.on_object_begin(key); }
    void on_object_end(string_view key) override { inner_.on_object_end(key); }
    void on_array_begin(string_view key) override { inner_.on_array_begin(key); }
    void on_array_end(string_view key) override { inner_.on_array_end(key); }
    void reset() override { inner_.reset(); }
};

// ---------------------------------------------------------------------------
// Template filter/capture sinks — zero vtable overhead.
// Used by the static (non-virtual) execute path.
// ---------------------------------------------------------------------------

/// Filter sink that forwards to a concrete inner sink (no vtable).
template<typename InnerT>
class FilterSink {
protected:
    InnerT& inner_;
public:
    explicit FilterSink(InnerT& inner) : inner_(inner) {}

    void on_null(string_view key) { inner_.on_null(key); }
    void on_bool(string_view key, bool value) { inner_.on_bool(key, value); }
    void on_number(string_view key, string_view raw) { inner_.on_number(key, raw); }
    void on_int(string_view key, int32_t value) { inner_.on_int(key, value); }
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
    void on_int(string_view, int32_t) {}
    void on_float(string_view, double) {}
    void on_string(string_view, string_view) {}
    void on_object_begin(string_view) {}
    void on_object_end(string_view) {}
    void on_array_begin(string_view) {}
    void on_array_end(string_view) {}
    void reset() {}
};

// ---------------------------------------------------------------------------
// BodyCaptureSink — accumulates the "body" sub-object as raw JSON.
//
// Generated sinks for endpoints with a body response inherit this.
// When on_object_begin("body") fires, all subsequent events are serialized
// into body_json until the matching on_object_end. The captured JSON is
// interned into the StringPool so it outlives the parser.
//
// Usage in generated sinks:
//   struct Sink : ::note::BodyCaptureSink {
//       void on_string(k, v) {
//           if (capture_body_event(...)) return;  // body handled
//           // normal field handling...
//       }
//       // same for on_bool, on_number, on_null, on_object_begin/end, on_array_begin/end
//   };
//   After parsing: rsp.body_json_ = sink.body_json;
// ---------------------------------------------------------------------------
struct BodyCaptureSink : DefaultSink {
    string_view body_json{};  // set after body object is fully captured

    // Returns true if the event was consumed by body capture.
    // Call from each on_* method in the generated sink.

    bool capture_body_object_begin(string_view key) {
        if (body_depth_ == 0 && key == "body") {
            body_depth_ = 1;
            body_size_ = 0;
            body_put_('{');
            return true;
        }
        if (body_depth_ > 0) {
            if (body_need_comma_) body_put_(',');
            if (!key.empty()) {
                body_put_('"');
                body_put_(key.data(), key.size());
                body_put_("\":", 2);
            }
            body_put_('{');
            ++body_depth_;
            body_need_comma_ = false;
            return true;
        }
        return false;
    }

    bool capture_body_object_end() {
        if (body_depth_ > 0) {
            body_put_('}');
            --body_depth_;
            body_need_comma_ = true;
            if (body_depth_ == 0) {
                // Data is already in arena memory — no intern needed
                body_json = string_view(body_data_, body_size_);
            }
            return true;
        }
        return false;
    }

    bool capture_body_array_begin(string_view key) {
        if (body_depth_ > 0) {
            if (body_need_comma_) body_put_(',');
            if (!key.empty()) {
                body_put_('"');
                body_put_(key.data(), key.size());
                body_put_("\":", 2);
            }
            body_put_('[');
            body_need_comma_ = false;
            return true;
        }
        return false;
    }

    bool capture_body_array_end() {
        if (body_depth_ > 0) {
            body_put_(']');
            body_need_comma_ = true;
            return true;
        }
        return false;
    }

    bool capture_body_string(string_view key, string_view value) {
        if (body_depth_ > 0) {
            emit_key_(key);
            body_put_('"');
            body_put_(value.data(), value.size());
            body_put_('"');
            body_need_comma_ = true;
            return true;
        }
        return false;
    }

    bool capture_body_number(string_view key, string_view raw) {
        if (body_depth_ > 0) {
            emit_key_(key);
            body_put_(raw.data(), raw.size());
            body_need_comma_ = true;
            return true;
        }
        return false;
    }

    bool capture_body_int(string_view key, int32_t value) {
        if (body_depth_ > 0) {
            emit_key_(key);
            char buf[12];
            auto n = detail::format_int(buf, value);
            body_put_(buf, n);
            body_need_comma_ = true;
            return true;
        }
        return false;
    }

    bool capture_body_float(string_view key, double value) {
        if (body_depth_ > 0) {
            emit_key_(key);
            char buf[24];
            auto n = detail::format_float(buf, value);
            body_put_(buf, n);
            body_need_comma_ = true;
            return true;
        }
        return false;
    }

    bool capture_body_bool(string_view key, bool value) {
        if (body_depth_ > 0) {
            emit_key_(key);
            body_put_(value ? "true" : "false", value ? 4 : 5);
            body_need_comma_ = true;
            return true;
        }
        return false;
    }

    bool capture_body_null(string_view key) {
        if (body_depth_ > 0) {
            emit_key_(key);
            body_put_("null", 4);
            body_need_comma_ = true;
            return true;
        }
        return false;
    }

    void reset() {
        body_json = {};
        body_depth_ = 0;
        body_data_ = nullptr;
        body_size_ = 0;
        body_cap_ = 0;
        body_need_comma_ = false;
    }

protected:
    StringPool* pool_ = nullptr;  // set by generated Sink constructor

private:
    char* body_data_ = nullptr;
    size_t body_size_ = 0;
    size_t body_cap_ = 0;
    int body_depth_ = 0;
    bool body_need_comma_ = false;

    void body_reserve_(size_t needed) {
        if (needed <= body_cap_) return;
        size_t new_cap = body_cap_ ? body_cap_ * 2 : 64;
        while (new_cap < needed) new_cap *= 2;
        body_data_ = static_cast<char*>(
            pool_->allocator().reallocate(body_data_, body_cap_, new_cap));
        body_cap_ = new_cap;
    }
    void body_put_(char c) {
        body_reserve_(body_size_ + 1);
        body_data_[body_size_++] = c;
    }
    void body_put_(const char* s, size_t n) {
        body_reserve_(body_size_ + n);
        std::memcpy(body_data_ + body_size_, s, n);
        body_size_ += n;
    }

    void emit_key_(string_view key) {
        if (body_need_comma_) body_put_(',');
        if (!key.empty()) {
            body_put_('"');
            body_put_(key.data(), key.size());
            body_put_("\":", 2);
        }
    }
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
    void on_int(string_view key, int32_t value) override { inner_.on_int(key, value); }
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

    void skip_ws() {
        while (pos_ < len_) {
            char c = json_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
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
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                              (h >= 'A' && h <= 'F')))
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
                        cp <<= 4;
                        char h = src[i];
                        if (h >= '0' && h <= '9') cp = static_cast<uint16_t>(cp | (h - '0'));
                        else if (h >= 'a' && h <= 'f') cp = static_cast<uint16_t>(cp | (h - 'a' + 10));
                        else if (h >= 'A' && h <= 'F') cp = static_cast<uint16_t>(cp | (h - 'A' + 10));
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

        if (c == '-' || (c >= '0' && c <= '9'))
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

// Helpers for parsing number strings delivered via on_number().
inline int32_t parse_int(string_view raw, int32_t def = 0) {
    if (raw.empty()) return def;
    int32_t result = 0;
    bool negative = false;
    size_t i = 0;
    if (raw[0] == '-') { negative = true; ++i; }
    for (; i < raw.size(); ++i) {
        char c = raw[i];
        if (c < '0' || c > '9') {
            if (c == '.') break;  // truncate decimal
            return def;
        }
        result = result * 10 + (c - '0');
    }
    return negative ? -result : result;
}

inline double parse_double(string_view raw, double def = 0.0) {
    if (raw.empty()) return def;
    // Simple parse for JSON numbers — handles integer, decimal, exponent.
    double result = 0.0;
    bool negative = false;
    size_t i = 0;
    if (raw[0] == '-') { negative = true; ++i; }
    for (; i < raw.size() && raw[i] != '.' && raw[i] != 'e' && raw[i] != 'E'; ++i)
        result = result * 10.0 + (raw[i] - '0');
    if (i < raw.size() && raw[i] == '.') {
        ++i;
        double frac = 0.1;
        for (; i < raw.size() && raw[i] != 'e' && raw[i] != 'E'; ++i) {
            result += (raw[i] - '0') * frac;
            frac *= 0.1;
        }
    }
    if (i < raw.size() && (raw[i] == 'e' || raw[i] == 'E')) {
        ++i;
        bool neg_exp = false;
        if (i < raw.size() && (raw[i] == '+' || raw[i] == '-')) {
            neg_exp = (raw[i] == '-');
            ++i;
        }
        int exp = 0;
        for (; i < raw.size() && raw[i] >= '0' && raw[i] <= '9'; ++i)
            exp = exp * 10 + (raw[i] - '0');
        double factor = 1.0;
        for (int j = 0; j < exp; ++j) factor *= 10.0;
        result = neg_exp ? result / factor : result * factor;
    }
    return negative ? -result : result;
}

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
