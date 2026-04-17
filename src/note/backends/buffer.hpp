// Zero-allocation JSON backend for note-cpp.
//
// Uses a fixed buffer for JSON building (no tree, no heap) and jsmn for
// zero-copy response parsing (tokens index into the original JSON string).
//
// Usage:
//   note::backends::BufferJsonBackend<512, 64> backend;
//   note::Notecard nc(backend, transport);
#pragma once

#define JSMN_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <note/backends/detail/jsmn.h>
#pragma GCC diagnostic pop

#include <note/json.hpp>
#include <note/json_buf.hpp>  // for note::detail::itoa, note::detail::dtoa

#include <cstring>
#include <memory>

namespace note::backends {

// ---------------------------------------------------------------------------
// BufferJsonBuilder: writes JSON directly into a fixed buffer.
// Same logic as JsonBuf<N> but with a runtime buffer pointer and virtual
// dispatch via the JsonBuilder interface.
// ---------------------------------------------------------------------------
class BufferJsonBuilder : public JsonBuilder {
public:
    using JsonBuilder::add;
    using JsonBuilder::add_element;

    BufferJsonBuilder(char* buf, size_t capacity)
        : buf_(buf), capacity_(capacity) { put('{'); }

    BufferJsonBuilder& add(string_view key, bool value) override {
        kv(key);
        put(value ? "true" : "false");
        return *this;
    }
    BufferJsonBuilder& add(string_view key, json_int_t value) override {
        kv(key);
        char tmp[24];
        size_t len = note::detail::itoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }
    BufferJsonBuilder& add(string_view key, double value) override {
        kv(key);
        char tmp[32];
        size_t len = note::detail::dtoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }
    BufferJsonBuilder& add(string_view key, string_view value) override {
        kv(key);
        quoted(value);
        return *this;
    }
    BufferJsonBuilder& add_raw(string_view key, string_view json_fragment) override {
        kv(key);
        for (char c : json_fragment) put(c);
        return *this;
    }
    BufferJsonBuilder& begin_object(string_view key) override {
        kv(key);
        put('{');
        need_comma_ = false;
        return *this;
    }
    BufferJsonBuilder& end_object() override {
        put('}');
        need_comma_ = true;
        return *this;
    }
    BufferJsonBuilder& begin_array(string_view key) override {
        kv(key);
        put('[');
        need_comma_ = false;
        return *this;
    }
    BufferJsonBuilder& end_array() override {
        put(']');
        need_comma_ = true;
        return *this;
    }
    BufferJsonBuilder& add_element(bool value) override {
        comma(); put(value ? "true" : "false"); return *this;
    }
    BufferJsonBuilder& add_element(json_int_t value) override {
        comma();
        char tmp[24];
        size_t len = note::detail::itoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }
    BufferJsonBuilder& add_element(double value) override {
        comma();
        char tmp[32];
        size_t len = note::detail::dtoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }
    BufferJsonBuilder& add_element(string_view value) override {
        comma(); quoted(value); return *this;
    }

    string_view to_view() override {
        close();
        return {buf_, pos_ < capacity_ ? pos_ : capacity_ - 1};
    }

    void reset() override {
        pos_ = 0;
        need_comma_ = false;
        closed_ = false;
        put('{');
    }

    bool overflow() const { return pos_ >= capacity_; }
    size_t size() const { return pos_; }

private:
    char* buf_;
    size_t capacity_;
    size_t pos_ = 0;
    bool need_comma_ = false;
    bool closed_ = false;

    void put(char c) {
        if (pos_ < capacity_ - 1) buf_[pos_] = c;
        ++pos_;
    }

    void put(string_view s) {
        for (char c : s) put(c);
    }

    void comma() {
        if (need_comma_) put(',');
        need_comma_ = true;
    }

    void quoted(string_view s) {
        put('"');
        for (char c : s) {
            switch (c) {
            case '"':  put('\\'); put('"'); break;
            case '\\': put('\\'); put('\\'); break;
            case '\n': put('\\'); put('n'); break;
            case '\r': put('\\'); put('r'); break;
            case '\t': put('\\'); put('t'); break;
            default:   put(c); break;
            }
        }
        put('"');
    }

    void kv(string_view key) {
        comma();
        quoted(key);
        put(':');
    }

    void close() {
        if (!closed_) {
            put('}');
            if (pos_ < capacity_) buf_[pos_] = '\0';
            closed_ = true;
        }
    }
};

// ---------------------------------------------------------------------------
// JsmnJsonReader: reads fields from jsmn tokens. Zero-copy — string_views
// point directly into the original JSON string.
// ---------------------------------------------------------------------------
class JsmnJsonReader : public JsonReader {
public:
    // Root reader: tokenizes json into the provided token buffer.
    JsmnJsonReader(const char* json, size_t len,
                   jsmntok_t* tokens, int max_tokens)
        : json_(json), json_len_(len)
        , tokens_(tokens), root_(0)
    {
        jsmn_parser parser;
        jsmn_init(&parser);
        token_count_ = jsmn_parse(&parser, json, len, tokens, static_cast<unsigned int>(max_tokens));
        if (token_count_ < 0) token_count_ = 0;
    }

    // Sub-reader: views a child object within the parent's token array.
    JsmnJsonReader(const char* json, size_t len,
                   const jsmntok_t* tokens, int count, int root)
        : json_(json), json_len_(len)
        , tokens_(tokens), token_count_(count), root_(root) {}

    bool has(string_view key) const override {
        return find_value(key) >= 0;
    }

    bool get_bool(string_view key, bool def) const override {
        int idx = find_value(key);
        if (idx < 0) return def;
        auto sv = tok_view(idx);
        if (sv == "true") return true;
        if (sv == "false") return false;
        return def;
    }

    json_int_t get_int(string_view key, json_int_t def) const override {
        int idx = find_value(key);
        if (idx < 0) return def;
        auto sv = tok_view(idx);
        return parse_int(sv, def);
    }

    double get_double(string_view key, double def) const override {
        int idx = find_value(key);
        if (idx < 0) return def;
        auto sv = tok_view(idx);
        return parse_double(sv, def);
    }

    string_view get_string(string_view key, string_view def = {}) const override {
        int idx = find_value(key);
        if (idx < 0) return def;
        if (tokens_[idx].type != JSMN_STRING) return def;
        return tok_view(idx);
    }

    size_t get_string_array(string_view key, string_view* out, size_t max) const override {
        int idx = find_value(key);
        if (idx < 0 || tokens_[idx].type != JSMN_ARRAY) return 0;
        int count = tokens_[idx].size;
        size_t n = 0;
        int elem = idx + 1;  // first element follows the array token
        for (int i = 0; i < count && n < max; ++i) {
            if (elem >= token_count_) break;
            if (tokens_[elem].type == JSMN_STRING)
                out[n++] = tok_view(elem);
            // Skip to next sibling (element + its children)
            elem += tok_span(elem);
        }
        return n;
    }

    size_t get_object_array(string_view key,
                            std::unique_ptr<JsonReader>* out, size_t max) const override {
        int idx = find_value(key);
        if (idx < 0 || tokens_[idx].type != JSMN_ARRAY) return 0;
        int count = tokens_[idx].size;
        size_t n = 0;
        int elem = idx + 1;
        for (int i = 0; i < count && n < max; ++i) {
            if (elem >= token_count_) break;
            if (tokens_[elem].type == JSMN_OBJECT)
                out[n++] = std::make_unique<JsmnJsonReader>(
                    json_, json_len_, tokens_, token_count_, elem);
            elem += tok_span(elem);
        }
        return n;
    }

    std::unique_ptr<JsonReader> get_object(string_view key) const override {
        int idx = find_value(key);
        if (idx < 0 || tokens_[idx].type != JSMN_OBJECT) return nullptr;
        return std::make_unique<JsmnJsonReader>(
            json_, json_len_, tokens_, token_count_, idx);
    }

    bool has_error() const override {
        return token_count_ == 0
            || tokens_[root_].type != JSMN_OBJECT;
    }

    string_view get_error() const override {
        if (has_error()) return "JSON parse error";
        int idx = find_value("err");
        if (idx >= 0 && tokens_[idx].type == JSMN_STRING)
            return tok_view(idx);
        return {};
    }

private:
    const char* json_;
    size_t json_len_;
    const jsmntok_t* tokens_;
    int token_count_ = 0;
    int root_;

    // Get string_view for a token.
    string_view tok_view(int idx) const {
        const auto& t = tokens_[idx];
        return {json_ + t.start, static_cast<size_t>(t.end - t.start)};
    }

    // Count total tokens consumed by token at idx (including children).
    int tok_span(int idx) const {
        if (idx >= token_count_) return 1;
        const auto& t = tokens_[idx];
        if (t.type == JSMN_OBJECT) {
            int span = 1;
            for (int i = 0; i < t.size; ++i) {
                span += tok_span(idx + span);  // key
                span += tok_span(idx + span);  // value
            }
            return span;
        }
        if (t.type == JSMN_ARRAY) {
            int span = 1;
            for (int i = 0; i < t.size; ++i)
                span += tok_span(idx + span);
            return span;
        }
        return 1;  // string or primitive
    }

    // Find the value token for a given key in the root object.
    // Returns token index of the value, or -1 if not found.
    int find_value(string_view key) const {
        if (token_count_ == 0 || root_ >= token_count_) return -1;
        const auto& root = tokens_[root_];
        if (root.type != JSMN_OBJECT) return -1;

        int idx = root_ + 1;
        for (int i = 0; i < root.size; ++i) {
            if (idx + 1 >= token_count_) break;
            // Key token
            if (tokens_[idx].type == JSMN_STRING && tok_view(idx) == key)
                return idx + 1;
            // Skip key + value
            idx++;  // skip key
            idx += tok_span(idx);  // skip value (may be nested)
        }
        return -1;
    }

    static json_int_t parse_int(string_view sv, json_int_t def) {
        if (sv.empty()) return def;
        json_int_t result = 0;
        bool negative = false;
        size_t i = 0;
        if (sv[0] == '-') { negative = true; ++i; }
        for (; i < sv.size(); ++i) {
            char c = sv[i];
            if (c < '0' || c > '9') {
                if (c == '.') break;  // truncate decimal part
                return def;
            }
            result = result * 10 + (c - '0');
        }
        return negative ? -result : result;
    }

    static double parse_double(string_view sv, double def) {
        if (sv.empty()) return def;
        // Simple strtod-like parse for JSON numbers.
        double result = 0.0;
        bool negative = false;
        size_t i = 0;
        if (sv[0] == '-') { negative = true; ++i; }
        for (; i < sv.size() && sv[i] != '.' && sv[i] != 'e' && sv[i] != 'E'; ++i)
            result = result * 10.0 + (sv[i] - '0');
        if (i < sv.size() && sv[i] == '.') {
            ++i;
            double frac = 0.1;
            for (; i < sv.size() && sv[i] != 'e' && sv[i] != 'E'; ++i) {
                result += (sv[i] - '0') * frac;
                frac *= 0.1;
            }
        }
        // Skip exponent handling for now — Notecard JSON rarely uses scientific notation.
        return negative ? -result : result;
    }
};

// ---------------------------------------------------------------------------
// BufferJsonBackend: ties builder and reader together. Zero heap allocation
// for building; jsmn tokens are a member array (no heap).
// ---------------------------------------------------------------------------
template<size_t BuildBufSize = 512, size_t MaxTokens = 64>
class BufferJsonBackend : public JsonBackend {
public:
    std::unique_ptr<JsonBuilder> create_builder() override {
        // Fallback: use get_builder() but wrap in a non-owning unique_ptr.
        // This is for backward compat with code that expects unique_ptr.
        return std::make_unique<BufferJsonBuilder>(build_buf_, BuildBufSize);
    }

    JsonBuilder& get_builder() override {
        builder_.reset();
        return builder_;
    }

    std::unique_ptr<JsonReader> parse_response(string_view json) override {
        return std::make_unique<JsmnJsonReader>(
            json.data(), json.size(), tokens_, MaxTokens);
    }

    // Zero-allocation reader reuse — re-parses into member tokens + reader.
    JsonReader& get_reader(string_view json) override {
        reader_ = JsmnJsonReader(json.data(), json.size(), tokens_, MaxTokens);
        return reader_;
    }

private:
    char build_buf_[BuildBufSize]{};
    BufferJsonBuilder builder_{build_buf_, BuildBufSize};
    jsmntok_t tokens_[MaxTokens]{};
    JsmnJsonReader reader_{nullptr, 0, tokens_, 0};
};

} // namespace note::backends
