#pragma once

/// @file
/// Compile-time body construction with runtime-substituted slots.
///
/// Four user-facing surfaces, all layered over one segment-walker:
///
///   1. `body_template<L>()` + `.with(...)`  — template literal +
///      positional values, two artifacts. Best when a shape is reused.
///
///   2. `make_body<L>(values...)`            — same template literal,
///      one call site. Sugar over (1).
///
///   3. `body_object{ "k"_k = v, ... }`       — init-list with UDL keys.
///      Reads most like a JSON literal at the call site.
///
///   4. `body_builder().add("k"_k, v)...`   — fluent builder pattern,
///      familiar to users of `note::body([&]{b.add(...);})`.
///
/// Nested objects (`body_object`) and arrays (`body_array`) compose as
/// field values and array elements to any depth.
///
/// All four lower to the same emit logic — a static byte pool plus a
/// list of typed slot positions. The pool is baked into `.rodata` at
/// compile time; runtime work is one memcpy of the pool segments plus
/// per-slot byte writes for the values. No SAX lexer, no per-field
/// virtual dispatch on the body-emit path.
///
/// Slot syntax (positional, for surfaces 1 and 2 only — digit 1..9):
///   `$N`   — int32
///   `$Nf`  — double
///   `$Nb`  — bool
///   `$Ns`  — string
///   `$No`  — object  (positional arg is a `body_object`)
///   `$Na`  — array   (positional arg is a `body_array`)
///
/// Wire format: the user-facing surface is wire-format-agnostic. The
/// build flag `NOTE_JSONB` selects whether the baked static pool and the
/// per-slot runtime bytes are JSONB opcodes (length-tagged binary) or
/// JSON text (`{`, `,"key":`, decimal/quoted values). The split lives
/// entirely in the `detail::wire` namespace; everything above it is
/// shared. `operator BodyValue()` (the `req.body(...)` integration) works
/// in both modes — it splices the rendered bytes through the builder's
/// `begin_raw_value`, which emits the key + any separator in the matching
/// format.
///
/// Status: 0.x — the surface set and names may still change. Disable the
/// whole family with `NOTE_NO_BODY_TEMPLATE` to reclaim the
/// `JsonBuilder::begin_raw_value` vtable slot in builds that don't use it.
///
/// Lifetime: string values are captured as `string_view`s — the
/// underlying chars must outlive the body object. Numeric and bool
/// values are captured by value; nested `body_object` / `body_array`
/// values are captured by value (so they live as long as the enclosing
/// body). The body behaves like `note::body([&]{...})`: when implicitly
/// converted to BodyValue in a `req.body(...).execute()` expression, the
/// temporary lives through the full-expression.
///
/// Endianness note (JSONB only): the double byte order matches the
/// host's IEEE 754 in-memory layout (same as
/// `StreamingJsonbBuilder::add(string_view, double)`). On
/// little-endian platforms (x86, ARM, AVR — every note-cpp target)
/// this is `bits & 0xff` at byte 0 through MSB at byte 7. A
/// big-endian port would diverge.

#include <note/body.hpp>
#include <note/compiler.hpp>   // NOTE_UNREACHABLE
#include <note/detail/number_format.hpp>  // detail::itoa / dtoa_shortest (JSON wire)
#include <note/json.hpp>
#include <note/jsonb.hpp>
#include <note/types.hpp>      // note::string_view

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#if __cplusplus < 202002L
#error "note/body_template.hpp requires C++20"
#endif

#if NOTE_NO_BODY_TEMPLATE
#error "note/body_template.hpp included but NOTE_NO_BODY_TEMPLATE is set — \
the compile-time body surfaces need JsonBuilder::begin_raw_value, which that \
flag removes. Drop NOTE_NO_BODY_TEMPLATE in builds that use body_template."
#endif

namespace note {

namespace detail {

// ── Building blocks shared across all four surfaces ──────────────────────

/// NTTP-friendly string literal wrapper. Lets a `const char[]` literal
/// pass as a class non-type template parameter.
template<std::size_t N>
struct fixed_string {
    char data[N]{};
    consteval fixed_string(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    constexpr std::size_t size() const { return N - 1; }  // exclude NUL
    constexpr char operator[](std::size_t i) const { return data[i]; }
};

/// Per-slot type tag. Drives the parser's placeholder layout and the
/// emit-time dispatch on per-slot byte writes.
enum class slot_type : std::uint8_t {
    Int32,    // runtime emits 4 LE bytes (kInt32 opcode is in the static pool)
    Double,   // runtime emits 8 host-LE bytes (kDouble in static pool)
    Bool,     // runtime emits 1 byte: kTrue or kFalse (NO opcode in static pool)
    String,   // runtime emits bytes + '\0' (kString in static pool)
    Nested,   // runtime calls value.emit_to(w) — the nested body/array emits
              // its own complete byte stream (object braces or array brackets,
              // keys, values). The outer static pool holds only this slot's
              // key bytes (object context) or nothing (array element); no
              // opcode prefix, no value bytes. Wire-format-agnostic: whichever
              // format the build targets, the nested value emits it.
};

/// A run of static bytes between (or around) slots.
struct segment {
    std::size_t offset;
    std::size_t length;
};

/// Compile-time schema — static byte pool + segment table + slot type list.
/// Same shape regardless of which surface produced it. Structural type
/// (only std::array + structural members), so it can pass as a class-type
/// non-type template parameter to `compiled_body`.
template<std::size_t StaticBytes, std::size_t SlotCount>
struct schema_data {
    std::array<std::uint8_t, StaticBytes>     static_pool{};
    std::array<segment, SlotCount + 1>        segments{};
    std::array<slot_type, SlotCount>          slot_types{};
};

/// Two-pass parse helper — used by both the template-literal parser and
/// the body_object/body_builder schema-from-pairs computation.
struct schema_writer {
    std::uint8_t* static_pool;
    segment*      segments;
    slot_type*    slot_types;
    std::size_t   static_pos;
    std::size_t   slot_pos;
    std::size_t   segment_start;

    constexpr void put(std::uint8_t b) {
        if (static_pool != nullptr) static_pool[static_pos] = b;
        ++static_pos;
    }
    constexpr void close_segment_and_record_slot(slot_type t) {
        if (segments != nullptr) {
            segments[slot_pos] = { segment_start, static_pos - segment_start };
        }
        if (slot_types != nullptr) {
            slot_types[slot_pos] = t;
        }
        ++slot_pos;
        segment_start = static_pos;
    }
    constexpr void close_final_segment() {
        if (segments != nullptr) {
            segments[slot_pos] = { segment_start, static_pos - segment_start };
        }
    }
};

// ── Wire-format policy ────────────────────────────────────────────────────
//
// The only place the two wire formats diverge. Under JSONB the static pool
// holds opcodes (kBeginObject, kItem+key+'\0', kInt32, …) and the runtime
// emits length-tagged binary values. Under JSON text the static pool holds
// punctuation (`{`, `,"key":`, `[`) and the runtime emits decimal/quoted
// text. `compiled_body` and the three schema builders go through these
// helpers so the structure-vs-format split stays in one spot.
//
// Each compile-time helper has a matching `*_bytes` size function so the
// `schema_data<StaticBytes, …>` static array can be sized exactly (a
// mismatch would corrupt segment offsets). The `first` flag selects whether
// a JSON separator comma is needed; under JSONB it is ignored.

namespace wire {

constexpr void begin_object(schema_writer& s) {
#if NOTE_JSONB == 1
    s.put(jsonb::kBeginObject);
#else
    s.put('{');
#endif
}
constexpr void end_object(schema_writer& s) {
#if NOTE_JSONB == 1
    s.put(jsonb::kEndObject);
#else
    s.put('}');
#endif
}
constexpr void begin_array(schema_writer& s) {
#if NOTE_JSONB == 1
    s.put(jsonb::kBeginArray);
#else
    s.put('[');
#endif
}
constexpr void end_array(schema_writer& s) {
#if NOTE_JSONB == 1
    s.put(jsonb::kEndArray);
#else
    s.put(']');
#endif
}

/// Emit a field key. JSONB: `kItem + key + '\0'`. JSON: `[,] "key":`.
constexpr void field_key(schema_writer& s, const char* key,
                         std::size_t n, bool first) {
#if NOTE_JSONB == 1
    (void)first;
    s.put(jsonb::kItem);
    for (std::size_t i = 0; i < n; ++i) s.put(static_cast<std::uint8_t>(key[i]));
    s.put('\0');
#else
    if (!first) s.put(',');
    s.put('"');
    for (std::size_t i = 0; i < n; ++i) s.put(static_cast<std::uint8_t>(key[i]));
    s.put('"');
    s.put(':');
#endif
}
/// Static-pool bytes a `field_key` with `first=true` occupies (no comma).
consteval std::size_t field_key_bytes(std::size_t n) {
#if NOTE_JSONB == 1
    return 1 + n + 1;          // kItem + key + '\0'
#else
    return 1 + n + 1 + 1;      // '"' + key + '"' + ':'
#endif
}

/// Value opcode prefix for a primitive slot (JSONB only). JSON emits none.
constexpr void value_opcode(schema_writer& s, slot_type t) {
#if NOTE_JSONB == 1
    if (t == slot_type::Int32)       s.put(jsonb::kInt32);
    else if (t == slot_type::Double) s.put(jsonb::kDouble);
    else if (t == slot_type::String) s.put(jsonb::kString);
    // Bool / Nested: none.
#else
    (void)s; (void)t;
#endif
}
consteval std::size_t value_opcode_bytes(slot_type t) {
#if NOTE_JSONB == 1
    return (t == slot_type::Int32 || t == slot_type::Double
            || t == slot_type::String) ? 1 : 0;
#else
    (void)t; return 0;
#endif
}

/// Array element separator. JSONB: none. JSON: `,` before non-first elements.
constexpr void element_sep(schema_writer& s, bool first) {
#if NOTE_JSONB == 1
    (void)s; (void)first;
#else
    if (!first) s.put(',');
#endif
}

/// Total separator bytes across `count` fields/elements (commas under JSON).
consteval std::size_t separator_bytes(std::size_t count) {
#if NOTE_JSONB == 1
    (void)count; return 0;
#else
    return count > 0 ? count - 1 : 0;
#endif
}

#if NOTE_JSONB != 1
/// Write a JSON-text quoted+escaped string (matches StreamingJsonBuilder).
inline void emit_json_string(JsonWriter& w, note::string_view s) {
    w.write("\"", 1);
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
        case '"':  w.write("\\\"", 2); break;
        case '\\': w.write("\\\\", 2); break;
        case '\n': w.write("\\n", 2);  break;
        case '\r': w.write("\\r", 2);  break;
        case '\t': w.write("\\t", 2);  break;
        default:   w.write(&c, 1);     break;
        }
    }
    w.write("\"", 1);
}
#endif

}  // namespace wire

// ── Shared emit machinery — one base for all four surfaces ────────────────

/// Walks `Schema`'s segments + slots interleaved, writing to a JsonWriter.
/// Each surface (body_template, body_object, body_builder) inherits from
/// a concrete `compiled_body<Schema, StoredArgs...>` for its specific
/// schema + value types. Identical schemas → identical instantiation →
/// shared code in `.rodata`.
template<auto Schema, typename... StoredArgs>
class compiled_body {
public:
    static_assert(sizeof...(StoredArgs) == Schema.slot_types.size(),
        "compiled_body: stored arg count must match schema slot count");

    /// Emit the rendered byte stream to the given writer.
    void emit_to(JsonWriter& w) const {
        emit_each_(w, std::make_index_sequence<sizeof...(StoredArgs)>{});
    }

    /// Implicit conversion to BodyValue — the integration point that
    /// makes `req.body(/* any of the four surfaces */).execute()` work.
    /// Works in both wire modes: the rendered byte stream is JSONB opcodes
    /// or JSON text depending on the build flag, and the splice goes through
    /// the builder's `begin_raw_value` either way.
    operator BodyValue() const {
        return BodyValue(
            static_cast<const void*>(this),
            &compiled_body::write_to_builder_);
    }

protected:
    std::tuple<StoredArgs...> values_;

    // Single ctor — when StoredArgs is empty, this acts as the default;
    // otherwise it constructs from the per-slot canonical values.
    constexpr explicit compiled_body(StoredArgs... vals)
        : values_{std::move(vals)...} {}

private:
    template<std::size_t I>
    static void write_segment_(JsonWriter& w) {
        constexpr auto seg = Schema.segments[I];
        if constexpr (seg.length > 0) {
            w.write(
                reinterpret_cast<const char*>(&Schema.static_pool[seg.offset]),
                seg.length);
        }
    }

    template<std::size_t I>
    void emit_slot_(JsonWriter& w) const {
        constexpr auto t = Schema.slot_types[I];
        const auto& value = std::get<I>(values_);
        if constexpr (t == slot_type::Int32) {
#if NOTE_JSONB == 1
            const auto uv = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(value));
            const std::uint8_t bytes[4] = {
                static_cast<std::uint8_t>(uv),
                static_cast<std::uint8_t>(uv >> 8),
                static_cast<std::uint8_t>(uv >> 16),
                static_cast<std::uint8_t>(uv >> 24),
            };
            w.write(reinterpret_cast<const char*>(bytes), 4);
#else
            char tmp[24];
            const std::size_t n = note::detail::itoa(
                tmp, sizeof(tmp),
                static_cast<note::json_int_t>(static_cast<std::int32_t>(value)));
            w.write(tmp, n);
#endif
        } else if constexpr (t == slot_type::Double) {
#if NOTE_JSONB == 1
            const auto bits = std::bit_cast<std::uint64_t>(
                static_cast<double>(value));
            std::uint8_t bytes[8];
            for (std::size_t k = 0; k < 8; ++k) {
                bytes[k] = static_cast<std::uint8_t>(bits >> (k * 8));
            }
            w.write(reinterpret_cast<const char*>(bytes), 8);
#else
            char tmp[32];
            const std::size_t n = note::detail::dtoa_shortest(
                tmp, sizeof(tmp), static_cast<double>(value));
            w.write(tmp, n);
#endif
        } else if constexpr (t == slot_type::Bool) {
#if NOTE_JSONB == 1
            const std::uint8_t byte = value ? jsonb::kTrue : jsonb::kFalse;
            w.write(reinterpret_cast<const char*>(&byte), 1);
#else
            const note::string_view s =
                value ? note::string_view("true") : note::string_view("false");
            w.write(s.data(), s.size());
#endif
        } else if constexpr (t == slot_type::String) {
#if NOTE_JSONB == 1
            const note::string_view sv = value;
            if (sv.size() > 0) w.write(sv.data(), sv.size());
            const std::uint8_t nul = 0;
            w.write(reinterpret_cast<const char*>(&nul), 1);
#else
            wire::emit_json_string(w, value);
#endif
        } else {  // Nested — the value emits its own complete byte stream.
            value.emit_to(w);
        }
    }

    template<std::size_t... Is>
    void emit_each_(JsonWriter& w, std::index_sequence<Is...>) const {
        write_segment_<0>(w);
        ((emit_slot_<Is>(w), write_segment_<Is + 1>(w)), ...);
    }

    /// BodyValue WriteFn. `begin_raw_value("body")` makes the builder emit
    /// the key + any separator in its own wire format (`kItem("body")` for
    /// JSONB, `[,]"body":` for JSON text) and hands back the writer; the
    /// rendered body bytes (the schema's `{…}` / opcodes) follow. No per-
    /// field virtual dispatch on the path.
    static void write_to_builder_(const void* ctx, string_view, JsonBuilder& b) {
        const auto& self = *static_cast<const compiled_body*>(ctx);
        JsonWriter* w = b.begin_raw_value("body");
        if (w == nullptr) {
            // Builder doesn't stream to a byte writer (tree-mode backend).
            // body_template requires a streaming builder.
            NOTE_UNREACHABLE();
        }
        self.emit_to(*w);
    }
};

// ── Nested-value detection ────────────────────────────────────────────────

/// A type is an "emittable body" if it exposes `emit_to(JsonWriter&) const` —
/// i.e. it is one of our compiled bodies/arrays (every surface inherits the
/// method from `compiled_body`). Used to route nested values to the `Nested`
/// slot type, where they emit their own complete byte stream.
template<typename T>
concept emittable_body = requires(const T& v, JsonWriter& w) {
    v.emit_to(w);
};

// ── Schema computation: from a template literal (surfaces 1, 2) ───────────

/// Walk a template literal char-by-char and write the schema into the
/// given `schema_writer`. Two passes share this — measure (null pointers)
/// then emit (real pointers).
template<fixed_string Tpl>
constexpr void parse_template_into(schema_writer& s) {
    constexpr auto N = Tpl.size();
    std::size_t i = 0;

    auto skip_ws = [&]() {
        while (i < N) {
            char c = Tpl[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    };

    skip_ws();
    if (i >= N || Tpl[i] != '{') return;
    ++i;
    wire::begin_object(s);

    bool first_field = true;
    while (true) {
        skip_ws();
        if (i >= N) break;
        if (Tpl[i] == '}') {
            ++i;
            wire::end_object(s);
            s.close_final_segment();
            return;
        }
        // `,` between fields in the *template source*. The wire separator
        // (JSON comma) is driven by `first_field`, captured below.
        if (!first_field) {
            if (Tpl[i] != ',') return;
            ++i;
            skip_ws();
        }
        const bool is_first = first_field;
        first_field = false;

        if (i >= N || Tpl[i] != '"') return;
        ++i;
        // Field key, in the active wire format (kItem+key+'\0' vs [,]"key":).
#if NOTE_JSONB == 1
        (void)is_first;
        s.put(jsonb::kItem);
#else
        if (!is_first) s.put(',');
        s.put('"');
#endif
        while (i < N && Tpl[i] != '"') {
            s.put(static_cast<std::uint8_t>(Tpl[i]));
            ++i;
        }
        if (i >= N) return;
        ++i;
#if NOTE_JSONB == 1
        s.put('\0');
#else
        s.put('"');
        s.put(':');
#endif

        skip_ws();
        if (i >= N || Tpl[i] != ':') return;
        ++i;
        skip_ws();

        if (i >= N || Tpl[i] != '$') return;
        ++i;
        if (i >= N) return;
        char d = Tpl[i];
        if (d < '1' || d > '9') return;
        ++i;

        char suffix = (i < N) ? Tpl[i] : '\0';
        // The value opcode prefix (JSONB only) goes in the static pool here;
        // `wire::value_opcode` is a no-op under JSON text.
        if (suffix == 'f') {
            ++i;
            wire::value_opcode(s, slot_type::Double);
            s.close_segment_and_record_slot(slot_type::Double);
        } else if (suffix == 'b') {
            ++i;
            wire::value_opcode(s, slot_type::Bool);
            s.close_segment_and_record_slot(slot_type::Bool);
        } else if (suffix == 's') {
            ++i;
            wire::value_opcode(s, slot_type::String);
            s.close_segment_and_record_slot(slot_type::String);
        } else if (suffix == 'o' || suffix == 'a') {
            // Object ($No) / array ($Na) slot. Both map to Nested — the
            // positional argument (a body_object / body_array) emits its own
            // complete byte stream, so the static pool holds only the field
            // key. The o/a suffix documents the author's intent; the actual
            // shape is whatever the argument is.
            ++i;
            wire::value_opcode(s, slot_type::Nested);  // no-op (no prefix)
            s.close_segment_and_record_slot(slot_type::Nested);
        } else {
            wire::value_opcode(s, slot_type::Int32);
            s.close_segment_and_record_slot(slot_type::Int32);
        }
    }
}

struct template_sizes {
    std::size_t static_byte_count;
    std::size_t slot_count;
};

template<fixed_string Tpl>
consteval template_sizes measure_template() {
    schema_writer st{nullptr, nullptr, nullptr, 0, 0, 0};
    parse_template_into<Tpl>(st);
    return {st.static_pos, st.slot_pos};
}

template<fixed_string Tpl>
consteval auto schema_from_template() {
    constexpr auto sz = measure_template<Tpl>();
    schema_data<sz.static_byte_count, sz.slot_count> r{};
    schema_writer st{
        r.static_pool.data(), r.segments.data(), r.slot_types.data(),
        0, 0, 0
    };
    parse_template_into<Tpl>(st);
    return r;
}

// ── field_pair + key_tag (used by surfaces 3 and 4) ───────────────────────

/// A compile-time key paired with a runtime value. The key + slot_type
/// are template parameters (so the structure flows in the type system);
/// `value` is the runtime storage.
template<fixed_string Key, slot_type T, typename V>
struct field_pair {
    static constexpr auto key = Key;
    static constexpr auto slot_t = T;
    using value_type = V;

    V value;
};

/// Tag value carrying a compile-time key. Produced by the `_k` UDL;
/// `operator=` overloads pair it with a runtime value to yield a
/// `field_pair`. Each `operator=` selects the slot_type based on the
/// argument type — int → Int32, float/double → Double, bool → Bool,
/// string_view-convertible → String.
template<fixed_string Key>
struct key_tag {
    // Numeric — anything integral except bool collapses to int32.
    template<typename T>
        requires (std::is_integral_v<std::decay_t<T>>
                  && !std::is_same_v<std::decay_t<T>, bool>)
    constexpr auto operator=(T v) const {
        return field_pair<Key, slot_type::Int32, std::int32_t>{
            static_cast<std::int32_t>(v)
        };
    }

    // Floating-point — float / double collapse to double.
    template<typename T>
        requires std::is_floating_point_v<std::decay_t<T>>
    constexpr auto operator=(T v) const {
        return field_pair<Key, slot_type::Double, double>{
            static_cast<double>(v)
        };
    }

    // Bool — exactly bool. No implicit conversions from int.
    constexpr auto operator=(bool v) const {
        return field_pair<Key, slot_type::Bool, bool>{v};
    }

    // String — any `string_view`-constructible value that isn't already
    // matched by the arithmetic overloads above (and isn't a nested body —
    // those match the `emittable_body` overload below).
    template<typename T>
        requires (std::is_constructible_v<note::string_view, T>
                  && !std::is_arithmetic_v<std::decay_t<T>>
                  && !emittable_body<std::decay_t<T>>)
    constexpr auto operator=(T&& v) const {
        return field_pair<Key, slot_type::String, note::string_view>{
            note::string_view(std::forward<T>(v))
        };
    }

    // Nested — a `body_object` / `body_array` (or any compiled body) becomes
    // an object/array-valued field. Stored by value; the nested value emits
    // its own bytes at slot position. Lifetime: nested body lives as long as
    // the enclosing one (it's a tuple member of the outer's storage).
    template<typename T>
        requires emittable_body<std::decay_t<T>>
    constexpr auto operator=(T&& v) const {
        return field_pair<Key, slot_type::Nested, std::decay_t<T>>{
            std::forward<T>(v)
        };
    }
};

// ── Schema computation from a Pair pack (surfaces 3 and 4) ────────────────

/// Per-pair static-pool bytes, excluding the leading separator (counted once
/// across the whole object as `wire::separator_bytes`): field key + value
/// opcode prefix. The wire helpers know the per-format layout.
template<typename Pair>
consteval std::size_t pair_static_bytes() {
    return wire::field_key_bytes(Pair::key.size())
         + wire::value_opcode_bytes(Pair::slot_t);
}

template<typename Pair>
constexpr void emit_pair_into(schema_writer& s) {
    // First field in the object ⇔ no slot recorded yet (no leading comma).
    wire::field_key(s, Pair::key.data, Pair::key.size(), /*first=*/s.slot_pos == 0);
    wire::value_opcode(s, Pair::slot_t);
    s.close_segment_and_record_slot(Pair::slot_t);
}

template<typename... Pairs>
consteval auto schema_from_pairs() {
    constexpr std::size_t static_bytes =
        1 /*{ / kBeginObject*/
        + (pair_static_bytes<Pairs>() + ... + 0)
        + wire::separator_bytes(sizeof...(Pairs))
        + 1 /*} / kEndObject*/;
    constexpr std::size_t slot_count = sizeof...(Pairs);

    schema_data<static_bytes, slot_count> r{};
    schema_writer st{
        r.static_pool.data(), r.segments.data(), r.slot_types.data(),
        0, 0, 0
    };
    wire::begin_object(st);
    (emit_pair_into<Pairs>(st), ...);
    wire::end_object(st);
    st.close_final_segment();
    return r;
}

// ── Value classification + array elements (surface for body_array) ───────

/// Map a raw user value type to its canonical slot_type + storage type.
/// One place, shared by body_array's element deduction. (key_tag uses the
/// same logical mapping via its `operator=` overload set; the rules match.)
template<typename Raw>
struct value_classify {
    using decayed = std::decay_t<Raw>;
    static constexpr slot_type slot =
        std::is_same_v<decayed, bool>     ? slot_type::Bool   :
        std::is_integral_v<decayed>       ? slot_type::Int32  :
        std::is_floating_point_v<decayed> ? slot_type::Double :
        emittable_body<decayed>           ? slot_type::Nested :
                                            slot_type::String;
    using type =
        std::conditional_t<slot == slot_type::Bool,   bool,
        std::conditional_t<slot == slot_type::Int32,  std::int32_t,
        std::conditional_t<slot == slot_type::Double, double,
        std::conditional_t<slot == slot_type::Nested, decayed,
                                                      note::string_view>>>>;
};

/// An array element — a keyless counterpart to `field_pair`. The slot_type
/// drives the static-pool opcode prefix; `value` is the runtime storage.
template<slot_type T, typename V>
struct element_value {
    static constexpr auto slot_t = T;
    using value_type = V;
    V value;
};

/// The canonical `element_value<...>` form for a raw user value type.
template<typename Raw>
using element_value_for_t =
    element_value<value_classify<Raw>::slot, typename value_classify<Raw>::type>;

/// Convert a raw user value to its canonical storage value.
template<typename Raw>
constexpr typename value_classify<Raw>::type canonical_value(Raw&& v) {
    using C = value_classify<Raw>;
    if constexpr (C::slot == slot_type::Int32) {
        return static_cast<std::int32_t>(v);
    } else if constexpr (C::slot == slot_type::Double) {
        return static_cast<double>(v);
    } else if constexpr (C::slot == slot_type::Bool) {
        return static_cast<bool>(v);
    } else if constexpr (C::slot == slot_type::Nested) {
        return std::forward<Raw>(v);
    } else {  // String
        return note::string_view(std::forward<Raw>(v));
    }
}

/// Per-element static-pool bytes, excluding the leading separator (counted
/// once across the array as `wire::separator_bytes`): just the value opcode
/// prefix under JSONB (Int32/Double/String), nothing under JSON text.
/// Arrays carry no keys.
template<typename Elem>
consteval std::size_t element_static_bytes() {
    return wire::value_opcode_bytes(Elem::slot_t);
}

template<typename Elem>
constexpr void emit_element_into(schema_writer& s) {
    // First element ⇔ no slot recorded yet (no leading comma under JSON).
    wire::element_sep(s, /*first=*/s.slot_pos == 0);
    wire::value_opcode(s, Elem::slot_t);
    s.close_segment_and_record_slot(Elem::slot_t);
}

template<typename... Elems>
consteval auto schema_from_array_elements() {
    constexpr std::size_t static_bytes =
        1 /*[ / kBeginArray*/
        + (element_static_bytes<Elems>() + ... + 0)
        + wire::separator_bytes(sizeof...(Elems))
        + 1 /*] / kEndArray*/;
    constexpr std::size_t slot_count = sizeof...(Elems);

    schema_data<static_bytes, slot_count> r{};
    schema_writer st{
        r.static_pool.data(), r.segments.data(), r.slot_types.data(),
        0, 0, 0
    };
    wire::begin_array(st);
    (emit_element_into<Elems>(st), ...);
    wire::end_array(st);
    st.close_final_segment();
    return r;
}

}  // namespace detail


// ── Surface 3: body_object — init-list with UDL keys ───────────────────────

/// A compile-time-structured body built from a list of UDL-keyed field
/// pairs. Each `"key"_k = value` produces a `field_pair`; the list is
/// aggregated by CTAD on the brace-init constructor.
///
/// Reads like a JSON literal at the call site:
/// ```
///   using namespace note::body_literals;
///   api.note.add().body(body_object{
///       "name"_k  = "station-7",
///       "seq"_k   = 42,
///       "temp"_k  = 22.5,
///       "alarm"_k = true,
///   }).execute();
/// ```
template<typename... Pairs>
class body_object
    : public detail::compiled_body<
          detail::schema_from_pairs<Pairs...>(),
          typename Pairs::value_type...>
{
    using base = detail::compiled_body<
        detail::schema_from_pairs<Pairs...>(),
        typename Pairs::value_type...>;

public:
    constexpr body_object(Pairs... pairs)
        : base{std::move(pairs.value)...} {}
};

// CTAD: deduce Pairs... from the brace-init args.
template<typename... Pairs>
body_object(Pairs...) -> body_object<Pairs...>;


/// A compile-time-structured array. Used as a field value inside a
/// `body_object` / `body_builder`, or nested inside another array. Elements
/// are heterogeneous and may themselves be `body_object` / `body_array`.
///
/// ```
///   using namespace note::body_literals;
///   api.note.add().body(body_object{
///       "tags"_k = body_array{"red", "green", "blue"},
///       "pts"_k  = body_array{
///           body_object{ "x"_k = 1, "y"_k = 2 },
///       },
///   }).execute();
/// ```
///
/// Lifetime: same contract as `body_object` — string elements capture
/// `string_view`s (the chars must outlive the array); nested bodies/arrays
/// are stored by value and live as long as the enclosing array.
template<typename... Elems>
class body_array
    : public detail::compiled_body<
          detail::schema_from_array_elements<Elems...>(),
          typename Elems::value_type...>
{
    using base = detail::compiled_body<
        detail::schema_from_array_elements<Elems...>(),
        typename Elems::value_type...>;

public:
    /// Construct from raw element values; each is canonicalised to its
    /// storage type (int→int32, float→double, char*→string_view, nested
    /// body/array forwarded by value).
    template<typename... Raw>
    constexpr explicit body_array(Raw&&... raw)
        : base{detail::canonical_value(std::forward<Raw>(raw))...} {}
};

// CTAD: deduce element types from the raw brace-init args.
template<typename... Raw>
body_array(Raw&&...) -> body_array<detail::element_value_for_t<Raw>...>;


// ── Surface 4: body_builder — fluent type-state ──────────────────────────

/// Fluent builder. Each `.add(key_tag, value)` returns a new builder
/// type with the field appended. Inherits the same emit machinery as
/// `body_object` — the builder *is* the body, no materialisation step.
/// Matches the existing `note::body([&]{b.add(...);})` mental model
/// but with compile-time structure.
///
/// ```
///   using namespace note::body_literals;
///   api.note.add().body(
///       body_builder()
///           .add("name"_k,  "station-7")
///           .add("seq"_k,   42)
///           .add("temp"_k,  22.5)
///           .add("alarm"_k, true)
///   ).execute();
/// ```
///
/// Lifetime: same contract as `body_object` and `note::body(lambda)` —
/// the builder must outlive the `BodyValue` that captures a pointer
/// to it. Chained `req.body(builder).execute()` is safe (the temporary
/// lives through the full-expression). Binding to a named variable
/// then passing the variable to `req.body(...)` is also safe.
///
/// POC scope: top-level fields only. begin_object / begin_array
/// are deferred to v0.next.
template<typename... Pairs>
class body_builder
    : public detail::compiled_body<
          detail::schema_from_pairs<Pairs...>(),
          typename Pairs::value_type...>
{
    using base = detail::compiled_body<
        detail::schema_from_pairs<Pairs...>(),
        typename Pairs::value_type...>;

public:
    // Single ctor — when Pairs is empty, this acts as the default;
    // otherwise it constructs from the canonical per-pair values.
    constexpr body_builder(typename Pairs::value_type... vals)
        : base{std::move(vals)...} {}

    /// Append a field. Returns a new builder type with the field added.
    template<detail::fixed_string Key, typename Value>
    constexpr auto add(detail::key_tag<Key>, Value&& v) const {
        auto new_pair = detail::key_tag<Key>{} = std::forward<Value>(v);
        return append_<decltype(new_pair)>(
            std::move(new_pair.value),
            std::make_index_sequence<sizeof...(Pairs)>{});
    }

private:
    template<typename NewPair, std::size_t... Is>
    constexpr auto append_(typename NewPair::value_type new_value,
                           std::index_sequence<Is...>) const {
        return body_builder<Pairs..., NewPair>(
            std::get<Is>(this->values_)...,
            std::move(new_value)
        );
    }
};

// CTAD for the no-args bootstrap.
body_builder() -> body_builder<>;


// ── Surface 1: body_template — compile-time template literal ──────────────

template<detail::fixed_string Tpl, typename... StoredArgs>
class body_template_call;

/// Compile-time-parsed body template (surface 1). Holds the schema
/// derived from the template literal. `with(...)` returns a
/// `body_template_call` capturing the runtime values; that converts
/// implicitly to BodyValue.
template<detail::fixed_string Tpl>
class body_template_t {
public:
    static constexpr auto data_ = detail::schema_from_template<Tpl>();
    static constexpr auto sizes_ = detail::template_sizes{
        data_.static_pool.size(),
        data_.slot_types.size()
    };

public:
    /// Capture per-slot runtime values into a `body_template_call`.
    /// Each argument's type must be compatible with its slot's declared
    /// type — int-like for `$N`, floating-point for `$Nf`, exactly
    /// `bool` for `$Nb`, `string_view`-constructible for `$Ns`, and a
    /// `body_object` / `body_array` for `$No` / `$Na`.
    ///
    /// The return type is deduced from the arguments: numeric/bool/string
    /// slots store their canonical type, while object/array slots store the
    /// argument's own type (each nested body/array is a distinct type, so
    /// there is no fixed alias to name — bind the result with `auto`).
    template<typename... Args>
        requires (sizeof...(Args) == sizes_.slot_count)
    constexpr auto with(Args&&... values) const {
        return make_call_(std::index_sequence_for<Args...>{},
                          std::forward<Args>(values)...);
    }

private:
    template<typename... Args, std::size_t... Is>
    static constexpr auto make_call_(std::index_sequence<Is...>,
                                     Args&&... values) {
        return body_template_call<
            Tpl, decltype(convert_arg_<Is, Args>(std::declval<Args>()))...>{
            convert_arg_<Is, Args>(std::forward<Args>(values))...
        };
    }

    template<std::size_t I, typename Arg>
    static constexpr auto convert_arg_(Arg&& value) {
        constexpr auto t = data_.slot_types[I];
        if constexpr (t == detail::slot_type::Int32) {
            static_assert(std::is_integral_v<std::decay_t<Arg>>,
                "int32 slot ($N) requires an integral argument");
            return static_cast<std::int32_t>(value);
        } else if constexpr (t == detail::slot_type::Double) {
            static_assert(std::is_floating_point_v<std::decay_t<Arg>>,
                "double slot ($Nf) requires a floating-point argument");
            return static_cast<double>(value);
        } else if constexpr (t == detail::slot_type::Bool) {
            static_assert(std::is_same_v<std::decay_t<Arg>, bool>,
                "bool slot ($Nb) requires a bool argument "
                "(no implicit conversions)");
            return static_cast<bool>(value);
        } else if constexpr (t == detail::slot_type::Nested) {
            static_assert(detail::emittable_body<std::decay_t<Arg>>,
                "object/array slot ($No / $Na) requires a body_object or "
                "body_array argument");
            return std::decay_t<Arg>{std::forward<Arg>(value)};
        } else {  // String
            static_assert(
                std::is_constructible_v<note::string_view, Arg>,
                "string slot ($Ns) requires a string_view-constructible "
                "argument (const char*, std::string, string_view, …)");
            return note::string_view(std::forward<Arg>(value));
        }
    }
};

/// Carrier for the values that fill a `body_template`'s slots. Inherits
/// from `compiled_body`, which provides emit_to + BodyValue conversion.
template<detail::fixed_string Tpl, typename... StoredArgs>
class body_template_call
    : public detail::compiled_body<body_template_t<Tpl>::data_, StoredArgs...>
{
    using base = detail::compiled_body<body_template_t<Tpl>::data_, StoredArgs...>;

public:
    constexpr explicit body_template_call(StoredArgs... vals)
        : base{std::move(vals)...} {}
};


// ── Factories ─────────────────────────────────────────────────────────────

/// Factory for `body_template_t<Tpl>` — surface 1's two-step pattern.
template<detail::fixed_string Tpl>
constexpr auto body_template() {
    return body_template_t<Tpl>{};
}

/// One-call template literal + values — surface 2. Sugar over
/// `body_template<Tpl>().with(values...)`. Equivalent behaviour, one
/// expression.
template<detail::fixed_string Tpl, typename... Args>
constexpr auto make_body(Args&&... values) {
    return body_template_t<Tpl>{}.with(std::forward<Args>(values)...);
}

}  // namespace note


// ── UDL: `"name"_k` produces a key_tag carrying the key as NTTP ───────────

namespace note::body_literals {

/// `"key"_k` returns a `key_tag<"key">{}` — used by surfaces 3 and 4.
/// Apply `operator=` to pair it with a runtime value: `"k"_k = 42` yields
/// a `field_pair<"k", slot_type::Int32, int32_t>{42}`.
template<detail::fixed_string S>
constexpr auto operator""_k() {
    return detail::key_tag<S>{};
}

}  // namespace note::body_literals
