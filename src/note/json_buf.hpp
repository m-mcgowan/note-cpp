#pragma once

#include <note/detail/number_format.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace note {


// ── JsonBuf ─────────────────────────────────────────────────────────────────
// A constexpr-capable JSON builder that writes directly to a fixed buffer.
//
// Same add()/begin_object()/end_object() shape as JsonBuilder, but:
//   - No virtual dispatch, no allocations
//   - constexpr: when all values are compile-time constants, the entire
//     JSON string is computed at compile time
//   - At runtime, writes are simple memcpy-like buffer fills
//
// N is the total buffer size including the null terminator. Typical values:
//   JsonBuf<64>  — small body fragments
//   JsonBuf<256> — single request
//   JsonBuf<512> — request with body
//
// Design note: JsonBuf<N> intentionally keeps all method bodies inside the
// template class rather than in a non-template base. A pointer-based base
// would break constexpr copy semantics (clang's constexpr evaluator rejects
// pointers whose provenance traces back to temporaries). For embedded targets
// where binary size matters, typical usage is 1-2 distinct N values, so the
// per-N instantiation cost is bounded and acceptable.
//
// Usage:
//   // Compile-time — guaranteed via json_const:
//   constexpr auto j = note::json_const([] {
//       note::JsonBuf<128> b;
//       b.add("req", "hub.set");
//       b.add("mode", "periodic");
//       b.close();
//       return b;
//   });
//
//   // Runtime (any value dynamic):
//   note::JsonBuf<256> b;
//   b.add("req", "note.add");
//   b.begin_object("body");
//       b.add("temp", sensor.read());
//   b.end_object();
//   b.close();
//   send(b.data(), b.size());
//
//   // Composable fragments:
//   auto body = note::JsonBuf<64>::object();
//   body.add("temp", 22.5);
//   body.close();
//   note::JsonBuf<256> req;
//   req.add("req", "note.add");
//   req.add("body", body);
//   req.close();

template<size_t N>
class JsonBuf {
    enum class Kind : char { Object, Array };

    char buf_[N]{};
    size_t pos_ = 0;
    Kind kind_ = Kind::Object;
    bool need_comma_ = false;
    bool closed_ = false;

    constexpr void put(char c) {
        if (pos_ < N - 1) buf_[pos_] = c;
        ++pos_;  // always count, even past capacity (snprintf convention)
    }

    constexpr void put(std::string_view s) {
        for (char c : s) put(c);
    }

    constexpr void put(const char* s) {
        while (*s) put(*s++);
    }

    constexpr void comma() {
        if (need_comma_) put(',');
        need_comma_ = true;
    }

    // Write a JSON-quoted string (with escaping).
    constexpr void quoted(std::string_view s) {
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

    constexpr void key(std::string_view k) {
        comma();
        quoted(k);
        put(':');
    }

    // Raw copy — no quoting, used for embedding fragments.
    constexpr void put_raw(const char* s, size_t len) {
        for (size_t i = 0; i < len; ++i) put(s[i]);
    }

    constexpr explicit JsonBuf(Kind k) : kind_(k) {
        put(k == Kind::Array ? '[' : '{');
    }

public:
    constexpr JsonBuf() { put('{'); }

    // ── Fragment factories ──────────────────────────────────────────────

    static constexpr JsonBuf object() { return JsonBuf{Kind::Object}; }
    static constexpr JsonBuf array()  { return JsonBuf{Kind::Array}; }

    // ── Scalars (keyed — for objects) ───────────────────────────────────

    constexpr JsonBuf& add(std::string_view k, std::string_view value) {
        key(k);
        quoted(value);
        return *this;
    }

    // Prevent const char* from matching bool.
    constexpr JsonBuf& add(std::string_view k, const char* value) {
        return add(k, std::string_view(value));
    }

    constexpr JsonBuf& add(std::string_view k, int32_t value) {
        key(k);
        char tmp[12]{};
        size_t len = detail::itoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }

    // Accept any integer type, widen to int32_t.
    template<typename T, std::enable_if_t<
        std::is_integral_v<T> && !std::is_same_v<T, bool> &&
        !std::is_same_v<T, int32_t> && !std::is_same_v<T, char>, int> = 0>
    constexpr JsonBuf& add(std::string_view k, T value) {
        return add(k, static_cast<int32_t>(value));
    }

    constexpr JsonBuf& add(std::string_view k, double value) {
        key(k);
        char tmp[32]{};
        size_t len = detail::dtoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }

    constexpr JsonBuf& add(std::string_view k, float value) {
        return add(k, static_cast<double>(value));
    }

    constexpr JsonBuf& add(std::string_view k, bool value) {
        key(k);
        put(value ? "true" : "false");
        return *this;
    }

    // ── Embed pre-built fragment (keyed) ────────────────────────────────

    template<size_t M>
    constexpr JsonBuf& add(std::string_view k, const JsonBuf<M>& fragment) {
        key(k);
        auto fv = fragment.view();
        put_raw(fv.data(), fv.size());
        return *this;
    }

    // ── Nested objects (inline) ─────────────────────────────────────────

    constexpr JsonBuf& begin_object(std::string_view k) {
        key(k);
        put('{');
        need_comma_ = false;
        return *this;
    }

    constexpr JsonBuf& end_object() {
        put('}');
        need_comma_ = true;
        return *this;
    }

    // ── Arrays (inline) ─────────────────────────────────────────────────

    constexpr JsonBuf& begin_array(std::string_view k) {
        key(k);
        put('[');
        need_comma_ = false;
        return *this;
    }

    constexpr JsonBuf& end_array() {
        put(']');
        need_comma_ = true;
        return *this;
    }

    // ── Array element adders (no key) ───────────────────────────────────

    constexpr JsonBuf& add(std::string_view value) {
        comma();
        quoted(value);
        return *this;
    }

    constexpr JsonBuf& add(const char* value) {
        return add(std::string_view(value));
    }

    constexpr JsonBuf& add(int32_t value) {
        comma();
        char tmp[12]{};
        size_t len = detail::itoa(tmp, sizeof(tmp), value);
        for (size_t i = 0; i < len; ++i) put(tmp[i]);
        return *this;
    }

    constexpr JsonBuf& add(bool value) {
        comma();
        put(value ? "true" : "false");
        return *this;
    }

    // Embed pre-built fragment as array element (no key).
    template<size_t M>
    constexpr JsonBuf& add(const JsonBuf<M>& fragment) {
        comma();
        auto fv = fragment.view();
        put_raw(fv.data(), fv.size());
        return *this;
    }

    // ── Finalize ────────────────────────────────────────────────────────

    // Close the object/array. Returns *this for chaining.
    constexpr JsonBuf& close() {
        if (!closed_) {
            put(kind_ == Kind::Array ? ']' : '}');
            if (pos_ < N) buf_[pos_] = '\0';
            closed_ = true;
        }
        return *this;
    }

    // ── Access ──────────────────────────────────────────────────────────

    constexpr const char* data() const { return buf_; }

    // Bytes needed (may exceed capacity on overflow — snprintf convention).
    constexpr size_t size() const { return pos_; }

    // Buffer capacity.
    static constexpr size_t capacity() { return N; }

    // True if the buffer was too small.
    constexpr bool overflow() const { return pos_ >= N; }

    constexpr explicit operator bool() const { return !overflow(); }

    // View of valid content (clamped to actual buffer on overflow).
    // Const: for consteval results where close() was called explicitly.
    constexpr std::string_view view() const & {
        return {buf_, pos_ < N ? pos_ : N - 1};
    }

    // Non-const: auto-closes before returning view.
    // Allows omitting close() for simple runtime use.
    constexpr std::string_view view() & {
        close();
        return {buf_, pos_ < N ? pos_ : N - 1};
    }
};


// ── json_const ──────────────────────────────────────────────────────────────
// Force compile-time evaluation. Compile error if any value is runtime.
// Also fails at compile time if the buffer overflows.
//
// Usage:
//   constexpr auto req = note::json_const([] {
//       note::JsonBuf<64> b;
//       b.add("req", "hub.set");
//       b.close();
//       return b;
//   });

#if __cplusplus >= 202002L

// consteval: only callable at compile time (C++20).
// GCC (--coverage) correctly excludes these from coverage metrics.
// Clang source-based coverage shows false-positive misses for consteval
// functions — use GCC for coverage reports on this codebase.
template<typename Fn>
consteval auto json_const(Fn fn) {
    auto result = fn();
    if (result.overflow()) throw "JsonBuf overflow: increase buffer size";
    return result;
}


// ── json ────────────────────────────────────────────────────────────────────
// Auto-sized compile-time JSON builder. The lambda receives a JsonBuf by
// reference — no buffer size needed. Measured in a first pass, then built
// with the exact size.
//
// Usage:
//   constexpr auto req = note::json<[](auto& b) {
//       b.add("req", "hub.set");
//       b.add("mode", "periodic");
//       b.close();
//   }>();
//
//   static_assert(req.view() == R"({"req":"hub.set","mode":"periodic"})");

// consteval: only callable at compile time (C++20).
// GCC (--coverage) correctly excludes these from coverage metrics.
// Clang source-based coverage shows false-positive misses for consteval
// functions — use GCC for coverage reports on this codebase.
template<auto fn>
consteval auto json() {
    // Pass 1: measure with a large probe buffer.
    constexpr size_t needed = [] {
        JsonBuf<4096> b;
        fn(b);
        return b.size();
    }();
    static_assert(needed <= 4096, "JSON exceeds 4096-byte probe buffer");
    // Pass 2: build with exact-sized buffer (+1 for null terminator).
    JsonBuf<needed + 1> b;
    fn(b);
    return b;
}

#endif // C++20

} // namespace note
