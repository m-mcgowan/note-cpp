#pragma once

#include "json.hpp"
#include "types.hpp"

#include <type_traits>
#include <utility>

// ── C++20 aggregate reflection ──────────────────────────────────────────────
// When C++20 is available, plain aggregate structs can be used as body values
// with zero boilerplate. Field names are extracted automatically.
//
// For C++17, use the NOTE_FIELDS() macro inside the struct definition.

#if __cplusplus >= 202002L
#define NTEST  // disable qlibs/reflect self-tests
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "third_party/reflect.hpp"
#pragma GCC diagnostic pop
#endif

namespace note {

// ── BodyValue ───────────────────────────────────────────────────────────────
// Type-erased body container.  Holds a reference to body data (string, lambda,
// or struct) and knows how to serialize it into a JsonBuilder.
//
// Lifetime contract: the referenced data must outlive the build() call.
// This is naturally satisfied by expression chains like:
//   api.noteAdd().set_body(readings).execute();

class BodyValue {
public:
    using WriteFn = void(*)(const void* ctx, string_view str, JsonBuilder& b);

    constexpr BodyValue() = default;

    // Tier 1: raw JSON string.
    BodyValue(string_view json)
        : str_(json), write_fn_(&write_string) {}

    // Prevent const char* from matching other constructors.
    BodyValue(const char* json)
        : str_(json), write_fn_(&write_string) {}

    // Tier 2/3: type-erased writer (used by body() helper and schema path).
    BodyValue(const void* ctx, WriteFn fn)
        : ctx_(ctx), write_fn_(fn) {}

    explicit operator bool() const { return write_fn_ != nullptr; }

    // Write the body into the parent JSON builder.
    void write_to(JsonBuilder& b) const {
        if (write_fn_) write_fn_(ctx_, str_, b);
    }

private:
    const void* ctx_ = nullptr;
    string_view str_{};
    WriteFn write_fn_ = nullptr;

    // Tier 1: pass raw JSON string to builder.
    static void write_string(const void*, string_view s, JsonBuilder& b) {
        b.add("body", s);
    }
};


// ── Tier 2: Builder (lambda) ────────────────────────────────────────────────
// Wrap a callable for deferred body construction.
//
// Usage:
//   req.set_body(note::body([](JsonBuilder& b) {
//       b.add("temp", 22.5);
//       b.add("humidity", 60);
//   }));

template<typename Fn>
BodyValue body(const Fn& fn) {
    return BodyValue(
        static_cast<const void*>(&fn),
        static_cast<BodyValue::WriteFn>([](const void* ctx, string_view, JsonBuilder& b) {
            b.begin_object("body");
            (*static_cast<const Fn*>(ctx))(b);
            b.end_object();
        })
    );
}


// ── Schema infrastructure ───────────────────────────────────────────────────
// Detect whether T has schema support (either via reflection or NOTE_FIELDS).

namespace detail {

// ── C++17 fallback: NOTE_FIELDS macro support ────────────────────────────────
// Trait for macro-registered schemas.
template<typename T, typename = void>
struct has_note_fields_trait : std::false_type {};

template<typename T>
struct has_note_fields_trait<T, std::void_t<decltype(T::_note_fields_write(
    std::declval<const T&>(), std::declval<JsonBuilder&>()))>>
    : std::true_type {};

#if __cplusplus >= 202002L

// C++20: detect aggregate types that can be reflected.
template<typename T>
concept ReflectableAggregate = std::is_aggregate_v<std::remove_cvref_t<T>>
    && !std::is_array_v<std::remove_cvref_t<T>>
    && !std::is_empty_v<std::remove_cvref_t<T>>
    && (reflect::size<std::remove_cvref_t<T>>() > 0);

// Forward declaration (write_field and write_aggregate are mutually recursive).
template<typename T>
void write_aggregate(const T& obj, JsonBuilder& b);

// Write a single field value to the builder.
template<typename V>
void write_field(JsonBuilder& b, string_view name, const V& value) {
    if constexpr (std::is_same_v<V, bool>) {
        b.add(name, value);
    } else if constexpr (std::is_integral_v<V>) {
        b.add(name, static_cast<int32_t>(value));
    } else if constexpr (std::is_floating_point_v<V>) {
        b.add(name, static_cast<double>(value));
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        b.add(name, string_view(value));
    } else if constexpr (ReflectableAggregate<V>) {
        // Nested aggregate: recurse.
        b.begin_object(name);
        write_aggregate(value, b);
        b.end_object();
    }
    // Unsupported types are silently skipped.
}

// Write all fields of an aggregate to the builder.
template<typename T>
void write_aggregate(const T& obj, JsonBuilder& b) {
    using R = std::remove_cvref_t<T>;
    [&]<std::size_t... Ns>(std::index_sequence<Ns...>) {
        (write_field(b, reflect::member_name<Ns, R>(), reflect::get<Ns>(obj)), ...);
    }(std::make_index_sequence<reflect::size<R>()>{});
}

// Concept: type that can be used as a body value.
template<typename T>
concept BodySchema = ReflectableAggregate<T> || has_note_fields_trait<T>::value;

#else // C++17

template<typename T, typename = void>
struct is_body_schema : has_note_fields_trait<T> {};

#endif

// ── Template type hints ─────────────────────────────────────────────────────
// Notecard templates use sample values to infer types:
//   float/double  → 14.1  (TFLOAT)
//   integer types → 1     (TINT8/16/32 depending on size)
//   bool          → true  (TBOOL)
//   string-like   → "1"   (TSTRING — value is max length as string)

#if __cplusplus >= 202002L

template<typename V>
void write_template_hint(JsonBuilder& b, string_view name, const V& /*unused*/) {
    if constexpr (std::is_same_v<V, bool>) {
        b.add(name, true);
    } else if constexpr (std::is_floating_point_v<V>) {
        b.add(name, 14.1);
    } else if constexpr (std::is_integral_v<V>) {
        // Use a representative value that hints at the storage size.
        if constexpr (sizeof(V) <= 1)
            b.add(name, static_cast<int32_t>(1));        // TINT8
        else if constexpr (sizeof(V) <= 2)
            b.add(name, static_cast<int32_t>(11));       // TINT16
        else
            b.add(name, static_cast<int32_t>(12));       // TINT32
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        b.add(name, string_view("1"));
    }
}

template<typename T>
void write_template_hints(JsonBuilder& b) {
    using R = std::remove_cvref_t<T>;
    [&]<std::size_t... Ns>(std::index_sequence<Ns...>) {
        (write_template_hint(b,
            reflect::member_name<Ns, R>(),
            reflect::get<Ns>(reflect::detail::ext<R>)), ...);
    }(std::make_index_sequence<reflect::size<R>()>{});
}

#endif // C++20

} // namespace detail


// ── make_schema_body ────────────────────────────────────────────────────────
// Create a BodyValue from a schema struct.

#if __cplusplus >= 202002L

template<typename T>
    requires detail::BodySchema<T>
BodyValue make_schema_body(const T& obj) {
    if constexpr (detail::has_note_fields_trait<T>::value) {
        // Use the NOTE_FIELDS macro-generated writer.
        return BodyValue(
            static_cast<const void*>(&obj),
            static_cast<BodyValue::WriteFn>([](const void* ctx, string_view, JsonBuilder& b) {
                b.begin_object("body");
                T::_note_fields_write(*static_cast<const T*>(ctx), b);
                b.end_object();
            })
        );
    } else {
        // Use C++20 reflection.
        return BodyValue(
            static_cast<const void*>(&obj),
            static_cast<BodyValue::WriteFn>([](const void* ctx, string_view, JsonBuilder& b) {
                b.begin_object("body");
                detail::write_aggregate(*static_cast<const T*>(ctx), b);
                b.end_object();
            })
        );
    }
}

#else // C++17

template<typename T,
    typename = std::enable_if_t<detail::has_note_fields_trait<T>::value>>
BodyValue make_schema_body(const T& obj) {
    return BodyValue(
        static_cast<const void*>(&obj),
        string_view{},
        [](const void* ctx, string_view, JsonBuilder& b) {
            b.begin_object("body");
            T::_note_fields_write(*static_cast<const T*>(ctx), b);
            b.end_object();
        }
    );
}

#endif


// ── template_of<T>() ────────────────────────────────────────────────────────
// Generate a BodyValue that writes Notecard template type hints.
//
// Usage:
//   api.noteTemplate().set()
//       .set_file("readings.qo")
//       .set_body(note::template_of<Readings>())
//       .execute();

#if __cplusplus >= 202002L

template<typename T>
    requires detail::ReflectableAggregate<T>
BodyValue template_of() {
    return BodyValue(
        nullptr,
        static_cast<BodyValue::WriteFn>([](const void*, string_view, JsonBuilder& b) {
            b.begin_object("body");
            detail::write_template_hints<T>(b);
            b.end_object();
        })
    );
}

/// Deduce T from an instance — avoids explicit template parameter.
///   Readings schema;
///   nc.note.templates().define("sensors.qo")
///       .body(note::template_of(schema))
///       .execute();
template<typename T>
    requires detail::ReflectableAggregate<T>
BodyValue template_of(const T&) {
    return template_of<T>();
}

#endif


// ── NOTE_FIELDS macro (C++17 fallback) ───────────────────────────────────────
// Usage:
//   struct Readings {
//       float temperature;
//       int16_t humidity;
//       NOTE_FIELDS(temperature, humidity)
//   };
//
// On C++20 and later, plain aggregates work automatically via reflection —
// this macro is only needed for C++17 compatibility.
//
// Generates static _note_fields_write() and _note_fields_read() methods
// that the BodyValue machinery detects and uses for serialization.

#define _NOTE_FIELDS_WRITE_FIELD(obj, b, field) \
    ::note::detail::_note_write_field(b, #field, (obj).field);

#define _NOTE_FIELDS_READ_FIELD(obj, r, field) \
    ::note::detail::_note_read_field(r, #field, (obj).field);

#define NOTE_FIELDS(...) \
    static void _note_fields_write(const auto& _self, ::note::JsonBuilder& _b) { \
        _NOTE_FIELDS_EXPAND(_NOTE_FIELDS_WRITE_EACH(_self, _b, __VA_ARGS__)) \
    } \
    static void _note_fields_read(auto& _self, const ::note::JsonReader& _r) { \
        _NOTE_FIELDS_EXPAND(_NOTE_FIELDS_READ_EACH(_self, _r, __VA_ARGS__)) \
    }

// Deprecated compatibility alias.
#define NOTE_BODY(...) NOTE_FIELDS(__VA_ARGS__)

// Macro helpers for field iteration (write path).
#define _NOTE_FIELDS_EXPAND(...) __VA_ARGS__
#define _NOTE_FIELDS_WRITE_EACH(obj, b, ...) \
    _NOTE_FIELDS_WRITE_MAP(obj, b, __VA_ARGS__)
#define _NOTE_FIELDS_WRITE_MAP(obj, b, ...) \
    _NOTE_FIELDS_WRITE_MAP_N(obj, b, __VA_ARGS__, \
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define _NOTE_FIELDS_WRITE_MAP_N(obj, b, \
    f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16, N, ...) \
    _NOTE_FIELDS_WRITE_MAP_##N(obj, b, \
        f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16)
#define _NOTE_FIELDS_WRITE_MAP_1(obj, b, f1, ...) \
    _NOTE_FIELDS_WRITE_FIELD(obj, b, f1)
#define _NOTE_FIELDS_WRITE_MAP_2(obj, b, f1, f2, ...) \
    _NOTE_FIELDS_WRITE_FIELD(obj, b, f1) _NOTE_FIELDS_WRITE_FIELD(obj, b, f2)
#define _NOTE_FIELDS_WRITE_MAP_3(obj, b, f1, f2, f3, ...) \
    _NOTE_FIELDS_WRITE_MAP_2(obj, b, f1, f2) _NOTE_FIELDS_WRITE_FIELD(obj, b, f3)
#define _NOTE_FIELDS_WRITE_MAP_4(obj, b, f1, f2, f3, f4, ...) \
    _NOTE_FIELDS_WRITE_MAP_3(obj, b, f1, f2, f3) _NOTE_FIELDS_WRITE_FIELD(obj, b, f4)
#define _NOTE_FIELDS_WRITE_MAP_5(obj, b, f1, f2, f3, f4, f5, ...) \
    _NOTE_FIELDS_WRITE_MAP_4(obj, b, f1, f2, f3, f4) _NOTE_FIELDS_WRITE_FIELD(obj, b, f5)
#define _NOTE_FIELDS_WRITE_MAP_6(obj, b, f1, f2, f3, f4, f5, f6, ...) \
    _NOTE_FIELDS_WRITE_MAP_5(obj, b, f1, f2, f3, f4, f5) _NOTE_FIELDS_WRITE_FIELD(obj, b, f6)
#define _NOTE_FIELDS_WRITE_MAP_7(obj, b, f1, f2, f3, f4, f5, f6, f7, ...) \
    _NOTE_FIELDS_WRITE_MAP_6(obj, b, f1, f2, f3, f4, f5, f6) _NOTE_FIELDS_WRITE_FIELD(obj, b, f7)
#define _NOTE_FIELDS_WRITE_MAP_8(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, ...) \
    _NOTE_FIELDS_WRITE_MAP_7(obj, b, f1, f2, f3, f4, f5, f6, f7) _NOTE_FIELDS_WRITE_FIELD(obj, b, f8)
#define _NOTE_FIELDS_WRITE_MAP_9(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, ...) \
    _NOTE_FIELDS_WRITE_MAP_8(obj, b, f1, f2, f3, f4, f5, f6, f7, f8) _NOTE_FIELDS_WRITE_FIELD(obj, b, f9)
#define _NOTE_FIELDS_WRITE_MAP_10(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, ...) \
    _NOTE_FIELDS_WRITE_MAP_9(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9) _NOTE_FIELDS_WRITE_FIELD(obj, b, f10)
#define _NOTE_FIELDS_WRITE_MAP_11(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, ...) \
    _NOTE_FIELDS_WRITE_MAP_10(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10) _NOTE_FIELDS_WRITE_FIELD(obj, b, f11)
#define _NOTE_FIELDS_WRITE_MAP_12(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, ...) \
    _NOTE_FIELDS_WRITE_MAP_11(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11) _NOTE_FIELDS_WRITE_FIELD(obj, b, f12)
#define _NOTE_FIELDS_WRITE_MAP_13(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, ...) \
    _NOTE_FIELDS_WRITE_MAP_12(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12) _NOTE_FIELDS_WRITE_FIELD(obj, b, f13)
#define _NOTE_FIELDS_WRITE_MAP_14(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, ...) \
    _NOTE_FIELDS_WRITE_MAP_13(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13) _NOTE_FIELDS_WRITE_FIELD(obj, b, f14)
#define _NOTE_FIELDS_WRITE_MAP_15(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, ...) \
    _NOTE_FIELDS_WRITE_MAP_14(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14) _NOTE_FIELDS_WRITE_FIELD(obj, b, f15)
#define _NOTE_FIELDS_WRITE_MAP_16(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, ...) \
    _NOTE_FIELDS_WRITE_MAP_15(obj, b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15) _NOTE_FIELDS_WRITE_FIELD(obj, b, f16)

// Macro helpers for field iteration (read path).
#define _NOTE_FIELDS_READ_EACH(obj, r, ...) \
    _NOTE_FIELDS_READ_MAP(obj, r, __VA_ARGS__)
#define _NOTE_FIELDS_READ_MAP(obj, r, ...) \
    _NOTE_FIELDS_READ_MAP_N(obj, r, __VA_ARGS__, \
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define _NOTE_FIELDS_READ_MAP_N(obj, r, \
    f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16, N, ...) \
    _NOTE_FIELDS_READ_MAP_##N(obj, r, \
        f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16)
#define _NOTE_FIELDS_READ_MAP_1(obj, r, f1, ...) \
    _NOTE_FIELDS_READ_FIELD(obj, r, f1)
#define _NOTE_FIELDS_READ_MAP_2(obj, r, f1, f2, ...) \
    _NOTE_FIELDS_READ_FIELD(obj, r, f1) _NOTE_FIELDS_READ_FIELD(obj, r, f2)
#define _NOTE_FIELDS_READ_MAP_3(obj, r, f1, f2, f3, ...) \
    _NOTE_FIELDS_READ_MAP_2(obj, r, f1, f2) _NOTE_FIELDS_READ_FIELD(obj, r, f3)
#define _NOTE_FIELDS_READ_MAP_4(obj, r, f1, f2, f3, f4, ...) \
    _NOTE_FIELDS_READ_MAP_3(obj, r, f1, f2, f3) _NOTE_FIELDS_READ_FIELD(obj, r, f4)
#define _NOTE_FIELDS_READ_MAP_5(obj, r, f1, f2, f3, f4, f5, ...) \
    _NOTE_FIELDS_READ_MAP_4(obj, r, f1, f2, f3, f4) _NOTE_FIELDS_READ_FIELD(obj, r, f5)
#define _NOTE_FIELDS_READ_MAP_6(obj, r, f1, f2, f3, f4, f5, f6, ...) \
    _NOTE_FIELDS_READ_MAP_5(obj, r, f1, f2, f3, f4, f5) _NOTE_FIELDS_READ_FIELD(obj, r, f6)
#define _NOTE_FIELDS_READ_MAP_7(obj, r, f1, f2, f3, f4, f5, f6, f7, ...) \
    _NOTE_FIELDS_READ_MAP_6(obj, r, f1, f2, f3, f4, f5, f6) _NOTE_FIELDS_READ_FIELD(obj, r, f7)
#define _NOTE_FIELDS_READ_MAP_8(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, ...) \
    _NOTE_FIELDS_READ_MAP_7(obj, r, f1, f2, f3, f4, f5, f6, f7) _NOTE_FIELDS_READ_FIELD(obj, r, f8)
#define _NOTE_FIELDS_READ_MAP_9(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, ...) \
    _NOTE_FIELDS_READ_MAP_8(obj, r, f1, f2, f3, f4, f5, f6, f7, f8) _NOTE_FIELDS_READ_FIELD(obj, r, f9)
#define _NOTE_FIELDS_READ_MAP_10(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, ...) \
    _NOTE_FIELDS_READ_MAP_9(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9) _NOTE_FIELDS_READ_FIELD(obj, r, f10)
#define _NOTE_FIELDS_READ_MAP_11(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, ...) \
    _NOTE_FIELDS_READ_MAP_10(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10) _NOTE_FIELDS_READ_FIELD(obj, r, f11)
#define _NOTE_FIELDS_READ_MAP_12(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, ...) \
    _NOTE_FIELDS_READ_MAP_11(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11) _NOTE_FIELDS_READ_FIELD(obj, r, f12)
#define _NOTE_FIELDS_READ_MAP_13(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, ...) \
    _NOTE_FIELDS_READ_MAP_12(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12) _NOTE_FIELDS_READ_FIELD(obj, r, f13)
#define _NOTE_FIELDS_READ_MAP_14(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, ...) \
    _NOTE_FIELDS_READ_MAP_13(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13) _NOTE_FIELDS_READ_FIELD(obj, r, f14)
#define _NOTE_FIELDS_READ_MAP_15(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, ...) \
    _NOTE_FIELDS_READ_MAP_14(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14) _NOTE_FIELDS_READ_FIELD(obj, r, f15)
#define _NOTE_FIELDS_READ_MAP_16(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, ...) \
    _NOTE_FIELDS_READ_MAP_15(obj, r, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15) _NOTE_FIELDS_READ_FIELD(obj, r, f16)

namespace detail {

// Helper used by NOTE_FIELDS macro — dispatches to the correct JsonBuilder::add overload.
template<typename V>
void _note_write_field(JsonBuilder& b, string_view name, const V& value) {
    if constexpr (std::is_same_v<V, bool>) {
        b.add(name, value);
    } else if constexpr (std::is_integral_v<V>) {
        b.add(name, static_cast<int32_t>(value));
    } else if constexpr (std::is_floating_point_v<V>) {
        b.add(name, static_cast<double>(value));
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        b.add(name, string_view(value));
    }
}

// Helper used by NOTE_FIELDS macro — reads a field from JsonReader into the target.
template<typename V>
void _note_read_field(const JsonReader& r, string_view name, V& out) {
    if constexpr (std::is_same_v<V, bool>) {
        out = r.get_bool(name);
    } else if constexpr (std::is_integral_v<V>) {
        out = static_cast<V>(r.get_int(name));
    } else if constexpr (std::is_floating_point_v<V>) {
        out = static_cast<V>(r.get_double(name));
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        out = V(r.get_string(name));
    }
}

} // namespace detail


// ── Response body parsing ───────────────────────────────────────────────────
// Parse a JSON body object into a schema struct.
//
// Usage:
//   auto reader = result->body();  // get body JsonReader
//   auto readings = note::parse<Readings>(*reader);

// C++17: parse types registered with NOTE_FIELDS macro.
template<typename T,
    typename = std::enable_if_t<detail::has_note_fields_trait<T>::value>>
T parse(const JsonReader& r) {
    T obj{};
    T::_note_fields_read(obj, r);
    return obj;
}

#if __cplusplus >= 202002L

namespace detail {

template<typename V>
V read_field(const JsonReader& r, string_view name) {
    if constexpr (std::is_same_v<V, bool>) {
        return r.get_bool(name);
    } else if constexpr (std::is_integral_v<V>) {
        return static_cast<V>(r.get_int(name));
    } else if constexpr (std::is_floating_point_v<V>) {
        return static_cast<V>(r.get_double(name));
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        return V(r.get_string(name));
    } else {
        return V{};  // unsupported type returns default
    }
}

} // namespace detail

// C++20: parse reflected aggregates (takes priority over macro version).
template<typename T>
    requires (detail::ReflectableAggregate<T> && !detail::has_note_fields_trait<T>::value)
T parse(const JsonReader& r) {
    T obj{};
    using R = std::remove_cvref_t<T>;
    [&]<std::size_t... Ns>(std::index_sequence<Ns...>) {
        ((reflect::get<Ns>(obj) = detail::read_field<reflect::member_type<Ns, R>>(
            r, reflect::member_name<Ns, R>())), ...);
    }(std::make_index_sequence<reflect::size<R>()>{});
    return obj;
}

#endif

} // namespace note
