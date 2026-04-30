#pragma once

/// @file body_bytes.hpp
/// BodyBytes — a JsonSink that re-serializes body events back to JSON text
/// in a caller-provided buffer.
///
/// Used with `req.into(body_bytes).execute()` to capture the response body
/// as raw JSON without parsing it into a typed struct. Works in both
/// streaming and buffered transport modes — the JSON is built up from SAX
/// events, so storage is exactly the caller's buffer (no intermediate tree).
///
/// The captured form wraps the body's contents in `{...}`, matching what
/// `result.body()` produces in tree mode. The closing `}` is written by the
/// first call to `view()`, so call `view()` after `execute()` returns.

#include <note/json_sax.hpp>
#include <note/span.hpp>
#include <note/types.hpp>
#include <note/detail/number_format.hpp>

#include <cstddef>

namespace note {

class BodyBytes : public JsonSink {
public:
    /// Construct a BodyBytes that writes into `buffer`. If the body's JSON
    /// exceeds `buffer.size()`, `truncated()` returns true and the buffer
    /// holds a prefix of the JSON (not necessarily a valid JSON document).
    explicit BodyBytes(span<char> buffer) : buffer_(buffer) {}

    /// Finalize and return the captured body JSON. First call writes the
    /// closing `}`. Subsequent calls return the same view.
    string_view view() {
        finalize();
        return {buffer_.data(), pos_ < buffer_.size() ? pos_ : buffer_.size()};
    }

    /// True if the body's JSON didn't fit in the caller's buffer.
    bool truncated() const { return truncated_; }

    /// Raw byte count written so far (may exceed buffer.size() if truncated).
    size_t size() const { return pos_; }

    void on_bool(string_view k, bool v)               override { kv(k); put(v ? "true" : "false"); need_comma_ = true; }
    void on_int(string_view k, json_int_t v)          override { kv(k); char t[24]; auto n = detail::itoa(t, sizeof(t), v); put({t, n}); need_comma_ = true; }
    void on_float(string_view k, double v)            override { kv(k); char t[24]; auto n = detail::dtoa(t, sizeof(t), v); put({t, n}); need_comma_ = true; }
    void on_number(string_view k, string_view raw)    override { kv(k); put(raw); need_comma_ = true; }
    void on_string(string_view k, string_view v)      override { kv(k); quoted(v); need_comma_ = true; }
    void on_object_begin(string_view k)               override { kv(k); put('{'); need_comma_ = false; }
    void on_object_end(string_view)                   override { put('}'); need_comma_ = true; }
    void on_array_begin(string_view k)                override { kv(k); put('['); need_comma_ = false; }
    void on_array_end(string_view)                    override { put(']'); need_comma_ = true; }
    void on_null(string_view k)                       override { kv(k); put("null"); need_comma_ = true; }

private:
    void put(char c) {
        if (pos_ < buffer_.size()) buffer_.data()[pos_] = c;
        else truncated_ = true;
        ++pos_;
    }
    void put(string_view s) { for (char c : s) put(c); }

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

    void start() {
        if (!started_) { put('{'); started_ = true; need_comma_ = false; }
    }

    /// Write `,"key":` for object members, or `,` for array elements (empty key).
    void kv(string_view key) {
        start();
        if (need_comma_) put(',');
        if (!key.empty()) { quoted(key); put(':'); }
    }

    void finalize() {
        if (finalized_) return;
        start();
        put('}');
        finalized_ = true;
    }

    span<char> buffer_;
    size_t pos_ = 0;
    bool truncated_ = false;
    bool started_ = false;
    bool finalized_ = false;
    bool need_comma_ = false;
};

} // namespace note
