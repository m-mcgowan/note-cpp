// Lightweight JSON field extraction for known response shapes.
//
// This is NOT a general-purpose JSON parser. It's a handful of
// constexpr helpers that search for `"key":value` patterns in a
// buffer of well-formed JSON and return substrings or typed values.
//
// Use when:
//  - You know the response shape at compile time.
//  - Flash is tight (AVR-class targets).
//  - You're willing to trade robustness for ~8 KB of flash savings
//    vs. the full SAX parser (json_sax.hpp).
//
// Do NOT use when:
//  - The input may be adversarial or malformed.
//  - You need to decode JSON string escapes (\n, \uXXXX, etc.).
//  - You need to distinguish `"foo"` as a key from `"foo"` as a
//    value. The scanner finds the FIRST occurrence of a key pattern;
//    it tracks strings to avoid matching inside them, but it does not
//    track object/array nesting of keys.
//
// For robust parsing, use `transact_dispatch` with a `JsonSink`.
#pragma once

#include <note/types.hpp>
#include <note/detail/number_parse.hpp>
#include <note/field_desc.hpp>
#include <note/progmem.hpp>

#include <type_traits>

namespace note::scan {

namespace detail {

constexpr bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr size_t skip_ws(string_view s, size_t pos) {
    while (pos < s.size() && is_ws(s[pos])) ++pos;
    return pos;
}

// Prefix comparison on raw pointers. string_view::compare(pos, n, sv)
// is nominally noexcept-safe but some toolchains (AVR libstdc++)
// emit an unresolved std::__throw_out_of_range reference. Rolling our
// own keeps the generated code exception-free.
constexpr bool matches_at(string_view haystack, size_t pos, string_view needle) {
    if (pos + needle.size() > haystack.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        if (haystack[pos + i] != needle[i]) return false;
    }
    return true;
}

// Prefix comparison with a FlashString needle. On AVR the needle bytes
// live in program memory, so each byte is read via pgm_read_byte; on
// non-Harvard platforms the macro collapses to a plain load.
inline bool matches_at(string_view haystack, size_t pos, FlashString needle) {
    if (pos + needle.size() > haystack.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
#if NOTE_PROGMEM
        char c = static_cast<char>(pgm_read_byte(needle.ptr + i));
#else
        char c = needle.ptr[i];
#endif
        if (haystack[pos + i] != c) return false;
    }
    return true;
}

// One-past-the-last char of the JSON value starting at pos.
// Handles strings, objects, arrays, and primitives (numbers/bool/null).
constexpr size_t value_end(string_view s, size_t pos) {
    if (pos >= s.size()) return pos;
    char c = s[pos];
    if (c == '"') {
        ++pos;
        while (pos < s.size()) {
            if (s[pos] == '\\' && pos + 1 < s.size()) { pos += 2; continue; }
            if (s[pos] == '"') return pos + 1;
            ++pos;
        }
        return pos;
    }
    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 1;
        ++pos;
        while (pos < s.size() && depth > 0) {
            char d = s[pos];
            if (d == '"') {
                ++pos;
                while (pos < s.size()) {
                    if (s[pos] == '\\' && pos + 1 < s.size()) { pos += 2; continue; }
                    if (s[pos] == '"') { ++pos; break; }
                    ++pos;
                }
                continue;
            }
            if (d == open) ++depth;
            else if (d == close) --depth;
            ++pos;
        }
        return pos;
    }
    // Primitive (number/bool/null): terminate at delimiter or whitespace.
    while (pos < s.size()) {
        char d = s[pos];
        if (d == ',' || d == '}' || d == ']' || is_ws(d)) break;
        ++pos;
    }
    return pos;
}

// Find the value associated with `key`. Returns the offset of the
// first non-whitespace character of the value, or string_view::npos.
//
// Templated on Key so the same loop serves `string_view` (RAM) keys
// and `FlashString` (PROGMEM) keys — overload resolution on
// `matches_at` picks the right byte-comparison path. Both key types
// expose `.size()`.
//
// Scans forward skipping over string values so keys inside string
// bodies are not false-matched. Does NOT track object/array depth —
// the first `"key":` at any depth wins.
template<class Key>
constexpr size_t find_value(string_view json, Key key) {
    size_t pos = 0;
    while (pos < json.size()) {
        // Find next opening quote.
        while (pos < json.size() && json[pos] != '"') ++pos;
        if (pos >= json.size()) return string_view::npos;

        // Try to match the key at this position.
        size_t key_start = pos + 1;
        if (key_start + key.size() + 1 <= json.size()
            && matches_at(json, key_start, key)
            && json[key_start + key.size()] == '"') {
            // Found `"key"`; look for colon.
            size_t after = key_start + key.size() + 1;
            size_t cpos = skip_ws(json, after);
            if (cpos < json.size() && json[cpos] == ':') {
                return skip_ws(json, cpos + 1);
            }
            // Not followed by ':' — treat as a string value, skip it.
        }

        // Skip over this string entirely before continuing the search,
        // so that keys inside string values do not false-match.
        pos = value_end(json, pos);
    }
    return string_view::npos;
}

} // namespace detail

/// Return the raw JSON substring of the value associated with `key`,
/// or an empty string_view if the key is not found.
/// For string values the result includes the surrounding quotes.
constexpr string_view field(string_view json, string_view key) {
    size_t vs = detail::find_value(json, key);
    if (vs == string_view::npos) return {};
    size_t ve = detail::value_end(json, vs);
    // Construct directly from (ptr, len): string_view::substr is specified
    // to throw std::out_of_range when pos > size(), which on AVR
    // libstdc++ pulls in std::__throw_out_of_range (unresolvable).
    return string_view(json.data() + vs, ve - vs);
}

/// Return the `{...}` substring for an object-valued key, or empty
/// if the key is missing or its value is not an object.
constexpr string_view object(string_view json, string_view key) {
    auto v = field(json, key);
    if (v.empty() || v.front() != '{') return {};
    return v;
}

/// Return the `[...]` substring for an array-valued key, or empty
/// if the key is missing or its value is not an array.
constexpr string_view array(string_view json, string_view key) {
    auto v = field(json, key);
    if (v.empty() || v.front() != '[') return {};
    return v;
}

/// Extract a typed value for `key`, returning `def` if the key is
/// missing or the value cannot be interpreted as `T`.
///
/// Supported `T`: `bool`, any integral type, any floating-point type,
/// and `string_view`. `T` is deduced from the default value, so the
/// usual call form is `scan::get(json, "k", 0.0f)`.
///
/// For `string_view`, the returned view points into `json` (without
/// surrounding quotes) and does NOT decode escape sequences.
template<class T>
constexpr T get(string_view json, string_view key, T def) {
    auto v = field(json, key);
    if (v.empty()) return def;
    if constexpr (std::is_same_v<T, bool>) {
        if (v == "true") return true;
        if (v == "false") return false;
        return def;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<T>(parse_int(v, static_cast<json_int_t>(def)));
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(parse_double(v, static_cast<double>(def)));
    } else if constexpr (std::is_same_v<T, string_view>) {
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            return string_view(v.data() + 1, v.size() - 2);
        }
        return def;
    } else {
        static_assert(!std::is_same_v<T, T>, "scan::get: unsupported T");
    }
}

/// Single-pass visitor over the top-level `"key":value` pairs in an
/// object-shaped JSON document. `visitor(key, value)` is called once
/// per pair, with `key` unquoted and `value` as the raw JSON substring
/// (strings retain their surrounding quotes; nested objects and arrays
/// are returned as a single substring and not descended into).
///
/// Input is expected to start with an opening `{`; leading whitespace
/// is skipped. The scan stops at the matching `}` or end of input.
template<class Visitor>
constexpr void for_each(string_view json, Visitor&& visitor) {
    size_t pos = detail::skip_ws(json, 0);
    if (pos >= json.size() || json[pos] != '{') return;
    ++pos;
    while (pos < json.size()) {
        pos = detail::skip_ws(json, pos);
        if (pos >= json.size() || json[pos] == '}') return;
        if (json[pos] != '"') return;  // malformed — bail out
        size_t key_start = pos + 1;
        size_t key_end = key_start;
        while (key_end < json.size() && json[key_end] != '"') ++key_end;
        if (key_end >= json.size()) return;
        pos = detail::skip_ws(json, key_end + 1);
        if (pos >= json.size() || json[pos] != ':') return;
        pos = detail::skip_ws(json, pos + 1);
        size_t val_end = detail::value_end(json, pos);
        visitor(string_view(json.data() + key_start, key_end - key_start),
                string_view(json.data() + pos, val_end - pos));
        pos = detail::skip_ws(json, val_end);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
}

/// Dispatch tags for `into()`. The default strategy (`walk`) makes a
/// single pass over the JSON and dispatches each pair to the struct;
/// `pick` iterates struct fields and does one field lookup per field
/// against the JSON. Both produce the same result — the tag selects
/// the implementation.
///
/// Pick is pay-for-use: its overload is only instantiated if the
/// `pick` tag is explicitly passed, so callers who stick to the
/// default pay nothing for the alternative.
struct walk_t {};
struct pick_t {};
inline constexpr walk_t walk{};
inline constexpr pick_t pick{};

/// Populate `obj` in a single scan pass. `T` must use `NOTE_FIELDS(...)`.
/// For each top-level key in `json` that matches a declared field name,
/// the field is parsed according to its C++ type:
///   - floating-point → parse_double
///   - integral (except bool) → parse_int
///   - bool → "true"/"false"
///   - string_view → quoted value without surrounding quotes
/// Unknown keys are ignored; missing fields are left untouched.
template<class T>
constexpr void into(string_view json, T& obj, walk_t = {}) {
    for_each(json, [&](string_view k, string_view v) {
        T::_note_fields_dispatch(obj, k, [&](auto& field) {
            using F = std::remove_reference_t<decltype(field)>;
            if constexpr (std::is_same_v<F, bool>) {
                if (v == "true")       field = true;
                else if (v == "false") field = false;
            } else if constexpr (std::is_integral_v<F>) {
                field = static_cast<F>(parse_int(v));
            } else if constexpr (std::is_floating_point_v<F>) {
                field = static_cast<F>(parse_double(v));
            } else if constexpr (std::is_same_v<F, string_view>) {
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                    field = string_view(v.data() + 1, v.size() - 2);
                }
            }
        });
    });
}

/// Alternative `into` strategy: iterates the NOTE_FIELDS descriptor
/// table and performs one field lookup per field against `json`,
/// dispatching on `FieldType`. Runtime cost is O(fields × json_size);
/// flash cost is whatever get_X variants the struct's types require.
template<class T>
inline void into(string_view json, T& obj, pick_t) {
    uint8_t n = 0;
    const FieldDesc* table = T::template _note_field_descs<T>(n);
    auto* base = reinterpret_cast<char*>(&obj);
    for (uint8_t i = 0; i < n; ++i) {
        FieldDesc d = ::note::detail::read_field_desc(&table[i]);
        char* p = base + d.offset;
        string_view v = field(json, d.name);
        if (v.empty()) continue;
        switch (d.type) {
            case FieldType::Bool:
                if (v == "true")       *reinterpret_cast<bool*>(p) = true;
                else if (v == "false") *reinterpret_cast<bool*>(p) = false;
                break;
            case FieldType::Int8:
                *reinterpret_cast<int8_t*>(p) = static_cast<int8_t>(parse_int(v));
                break;
            case FieldType::Int16:
                *reinterpret_cast<int16_t*>(p) = static_cast<int16_t>(parse_int(v));
                break;
            case FieldType::Int32:
                *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(parse_int(v));
                break;
            case FieldType::Int:
                *reinterpret_cast<json_int_t*>(p) = parse_int(v);
                break;
            case FieldType::Float32:
                *reinterpret_cast<float*>(p) = static_cast<float>(parse_double(v));
                break;
            case FieldType::Double:
                *reinterpret_cast<double*>(p) = parse_double(v);
                break;
            case FieldType::String:
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                    *reinterpret_cast<string_view*>(p) =
                        string_view(v.data() + 1, v.size() - 2);
                }
                break;
        }
    }
}

// Named variants — unambiguous, no need to think about default literal type.
// All delegate to get<T>.
constexpr json_int_t  get_int   (string_view json, string_view key, json_int_t def = 0)        { return get<json_int_t>(json, key, def); }
constexpr double      get_double(string_view json, string_view key, double def = 0.0)          { return get<double>(json, key, def); }
constexpr float       get_float (string_view json, string_view key, float def = 0.0f)          { return get<float>(json, key, def); }
constexpr bool        get_bool  (string_view json, string_view key, bool def = false)          { return get<bool>(json, key, def); }
constexpr string_view get_str   (string_view json, string_view key, string_view def = {})      { return get<string_view>(json, key, def); }

// ---------------------------------------------------------------------------
// Flash-key overloads (AVR-friendly) — keys stored in program memory.
//
// Same semantics as the string_view overloads above; the key is read
// byte-at-a-time from flash via `pgm_read_byte` on AVR. On non-Harvard
// platforms the read collapses to a plain load (zero overhead). See
// docs/internal/avr-flash-strings.md for the design rationale and
// breakeven math.
// ---------------------------------------------------------------------------

inline string_view field(string_view json, FlashString key) {
    size_t vs = detail::find_value(json, key);
    if (vs == string_view::npos) return {};
    size_t ve = detail::value_end(json, vs);
    return string_view(json.data() + vs, ve - vs);
}

inline string_view object(string_view json, FlashString key) {
    auto v = field(json, key);
    if (v.empty() || v.front() != '{') return {};
    return v;
}

inline string_view array(string_view json, FlashString key) {
    auto v = field(json, key);
    if (v.empty() || v.front() != '[') return {};
    return v;
}

template<class T>
inline T get(string_view json, FlashString key, T def) {
    auto v = field(json, key);
    if (v.empty()) return def;
    if constexpr (std::is_same_v<T, bool>) {
        if (v == "true") return true;
        if (v == "false") return false;
        return def;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<T>(parse_int(v, static_cast<json_int_t>(def)));
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(parse_double(v, static_cast<double>(def)));
    } else if constexpr (std::is_same_v<T, string_view>) {
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            return string_view(v.data() + 1, v.size() - 2);
        }
        return def;
    } else {
        static_assert(!std::is_same_v<T, T>, "scan::get: unsupported T");
    }
}

inline json_int_t  get_int   (string_view json, FlashString key, json_int_t def = 0)        { return get<json_int_t>(json, key, def); }
inline double      get_double(string_view json, FlashString key, double def = 0.0)          { return get<double>(json, key, def); }
inline float       get_float (string_view json, FlashString key, float def = 0.0f)          { return get<float>(json, key, def); }
inline bool        get_bool  (string_view json, FlashString key, bool def = false)          { return get<bool>(json, key, def); }
inline string_view get_str   (string_view json, FlashString key, string_view def = {})      { return get<string_view>(json, key, def); }

} // namespace note::scan
