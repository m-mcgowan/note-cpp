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

#include "types.hpp"

#include <cstddef>

namespace note {

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
    virtual void on_string(string_view key, string_view value) { (void)key; (void)value; }

    // Called when a nested object begins/ends. The parser delivers the object's
    // key-value pairs between begin/end. For flat objects, these are only
    // called for the root.
    virtual void on_object_begin(string_view key) { (void)key; }
    virtual void on_object_end(string_view key) { (void)key; }

    // Called when an array begins/ends.
    virtual void on_array_begin(string_view key) { (void)key; }
    virtual void on_array_end(string_view key) { (void)key; }
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

        if (json_[pos_] != '{') return "expected object";
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
                if (pos_ >= len_) return "unexpected end in escape";
                char esc = json_[pos_];
                if (esc == 'u') {
                    // Skip \uXXXX
                    ++pos_;
                    for (int i = 0; i < 4 && pos_ < len_; ++i, ++pos_) {
                        char h = json_[pos_];
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                              (h >= 'A' && h <= 'F')))
                            return "invalid hex in \\u escape";
                    }
                    continue;
                }
                // Other escapes: just skip the escaped char
            }
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t')
                return "unescaped control character in string";
            ++pos_;
        }
        return "unterminated string";
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
                        if (h >= '0' && h <= '9') cp |= (h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
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
            if (advance() != ':') return "expected ':'";
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
            if (c != ',') return "expected ',' or '}'";
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
            if (c != ',') return "expected ',' or ']'";
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

        return "unexpected character";
    }

    string_view parse_literal(const char* lit, size_t lit_len,
                               string_view key, bool value) {
        if (pos_ + lit_len > len_) return "unexpected end";
        for (size_t i = 0; i < lit_len; ++i) {
            if (json_[pos_ + i] != lit[i]) return "invalid literal";
        }
        pos_ += lit_len;
        sink_.on_bool(key, value);
        return {};
    }

    string_view parse_null(string_view key) {
        if (pos_ + 4 > len_) return "unexpected end";
        if (json_[pos_] != 'n' || json_[pos_+1] != 'u' ||
            json_[pos_+2] != 'l' || json_[pos_+3] != 'l')
            return "invalid literal";
        pos_ += 4;
        sink_.on_null(key);
        return {};
    }

    string_view parse_number(string_view key) {
        size_t start = pos_;
        if (peek() == '-') ++pos_;

        if (pos_ >= len_) return "unexpected end in number";

        // Integer part
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (pos_ < len_ && json_[pos_] >= '0' && json_[pos_] <= '9')
                ++pos_;
        } else {
            return "invalid number";
        }

        // Fractional part
        if (pos_ < len_ && json_[pos_] == '.') {
            ++pos_;
            if (pos_ >= len_ || json_[pos_] < '0' || json_[pos_] > '9')
                return "invalid number: digit expected after '.'";
            while (pos_ < len_ && json_[pos_] >= '0' && json_[pos_] <= '9')
                ++pos_;
        }

        // Exponent
        if (pos_ < len_ && (json_[pos_] == 'e' || json_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < len_ && (json_[pos_] == '+' || json_[pos_] == '-'))
                ++pos_;
            if (pos_ >= len_ || json_[pos_] < '0' || json_[pos_] > '9')
                return "invalid number: digit expected in exponent";
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
// ---------------------------------------------------------------------------
class ErrorCaptureSink : public JsonSink {
    JsonSink& inner_;
    string_view err_;
public:
    explicit ErrorCaptureSink(JsonSink& inner) : inner_(inner) {}

    string_view captured_error() const { return err_; }

    void on_null(string_view key) override { inner_.on_null(key); }
    void on_bool(string_view key, bool value) override { inner_.on_bool(key, value); }
    void on_number(string_view key, string_view raw) override { inner_.on_number(key, raw); }
    void on_string(string_view key, string_view value) override {
        if (key == "err") { err_ = value; return; }
        inner_.on_string(key, value);
    }
    void on_object_begin(string_view key) override { inner_.on_object_begin(key); }
    void on_object_end(string_view key) override { inner_.on_object_end(key); }
    void on_array_begin(string_view key) override { inner_.on_array_begin(key); }
    void on_array_end(string_view key) override { inner_.on_array_end(key); }
};

}  // namespace note
