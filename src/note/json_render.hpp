#pragma once

#include <note/detail/number_format.hpp>
#include <note/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace note {

// ── JsonRender ──────────────────────────────────────────────────────────────
// Same JSON-shape API as JsonBuf<N>, but writes into an *external* buffer
// supplied at construction. Designed for the in-place transact pattern,
// where the same buffer is used to render the request and then to receive
// the response (saving N bytes of RAM versus the separate-buffer pattern).
//
// Differences from JsonBuf:
//   - Buffer is external (`char* buf, size_t cap`) — JsonRender does not
//     own storage.
//   - Not constexpr-capable (the constexpr evaluator cannot trace pointer
//     provenance through external storage).
//   - No `object()` / `array()` factories — the underlying buffer is always
//     reset to a top-level object on construction. Use JsonBuf<N> when you
//     want a composable fragment.
//
// Usage (typically inside `nc.transact_raw_inplace`):
//
//   nc.transact_raw_inplace(buf, [&](auto& w) {
//       w.add("req", "card.temp");
//   });
//
// Or directly:
//
//   char buf[64];
//   note::JsonRender w(buf, sizeof(buf));
//   w.add("req", "note.add");
//   w.begin_object("body");
//       w.add("temperature", sensor.read());
//   w.end_object();
//   w.close();
//   send(w.data(), w.size());
class JsonRender {
public:
    JsonRender(char* buf, size_t cap) : buf_(buf), cap_(cap) {
        put('{');
    }

    // ── Scalars (keyed) ─────────────────────────────────────────────────────

    JsonRender& add(std::string_view k, std::string_view value) {
        key(k);
        quoted(value);
        return *this;
    }

    // Prevent const char* from matching bool.
    JsonRender& add(std::string_view k, const char* value) {
        return add(k, std::string_view(value));
    }

    JsonRender& add(std::string_view k, json_int_t value) {
        key(k);
        char tmp[24]{};
        size_t len = detail::itoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }

    template<typename T, std::enable_if_t<
        std::is_integral_v<T> && !std::is_same_v<T, bool> &&
        !std::is_same_v<T, json_int_t> && !std::is_same_v<T, char>, int> = 0>
    JsonRender& add(std::string_view k, T value) {
        return add(k, static_cast<json_int_t>(value));
    }

    JsonRender& add(std::string_view k, double value) {
        key(k);
        char tmp[32]{};
        size_t len = detail::dtoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }

    JsonRender& add(std::string_view k, float value) {
        return add(k, static_cast<double>(value));
    }

    JsonRender& add(std::string_view k, bool value) {
        key(k);
        put(value ? "true" : "false");
        return *this;
    }

    // ── Nested objects (inline) ─────────────────────────────────────────────

    JsonRender& begin_object(std::string_view k) {
        key(k);
        put('{');
        need_comma_ = false;
        return *this;
    }

    JsonRender& end_object() {
        put('}');
        need_comma_ = true;
        return *this;
    }

    // ── Finalize ────────────────────────────────────────────────────────────

    JsonRender& close() {
        if (!closed_) {
            put('}');
            if (pos_ < cap_) buf_[pos_] = '\0';
            closed_ = true;
        }
        return *this;
    }

    // ── Access ──────────────────────────────────────────────────────────────

    const char* data() const { return buf_; }
    size_t size() const { return pos_; }
    size_t capacity() const { return cap_; }

    // True if the rendered JSON exceeded the buffer (snprintf convention —
    // pos_ keeps incrementing even past cap_).
    bool overflow() const { return pos_ > cap_; }

    std::string_view view() const {
        return {buf_, pos_ <= cap_ ? pos_ : cap_};
    }

private:
    void put(char c) {
        if (pos_ < cap_ - 1) buf_[pos_] = c;
        ++pos_;  // always count, even past capacity (snprintf convention)
    }

    void put(std::string_view s) { for (char c : s) put(c); }
    void put(const char* s) { while (*s) put(*s++); }

    void comma() {
        if (need_comma_) put(',');
        need_comma_ = true;
    }

    void quoted(std::string_view s) {
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

    void key(std::string_view k) {
        comma();
        quoted(k);
        put(':');
    }

    char* buf_;
    size_t cap_;
    size_t pos_ = 0;
    bool need_comma_ = false;
    bool closed_ = false;
};

} // namespace note
