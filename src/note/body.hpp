#pragma once

#include "field_desc.hpp"
#include "json.hpp"
#include "json_validate.hpp"
#include "types.hpp"
#include "wire_format.hpp"

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

// ── NOTE_STRICT_BODY_FIELDS ────────────────────────────────────────────────
// When 1 (default), unsupported struct field types in ser/deser/template
// paths trigger a compile-time static_assert rather than being silently
// dropped. When 0, the assertion is replaced by a #pragma message and
// the silent-drop behaviour from before this feature is preserved.
#ifndef NOTE_STRICT_BODY_FIELDS
#define NOTE_STRICT_BODY_FIELDS 1
#endif

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

#if NOTE_JSONB
    // Raw JSON string bodies are not supported with JSONB wire format.
    // Use body() with a lambda or typed struct instead.
#elif __cplusplus >= 202002L && !defined(__clang__) && !(defined(__GNUC__) && __GNUC__ < 14)
    // String literal: validated at compile time as well-formed JSON object.
    // Excluded from GCC < 14: inherited consteval constructors are broken (PR 102933).
    template<std::size_t N>
    consteval BodyValue(const char (&s)[N])
        : str_(string_view(s, N - 1)), write_fn_(&write_string) {
        if (!json_valid(string_view(s, N - 1)))
            throw "body: invalid JSON or not a top-level object";
    }

    // Runtime string_view (no validation — for dynamic/forwarded JSON).
    template<typename U>
        requires std::is_convertible_v<U, string_view>
              && (!std::is_array_v<std::remove_reference_t<U>>)
    constexpr BodyValue(U&& v)
        : str_(string_view(std::forward<U>(v))), write_fn_(&write_string) {}
#else
    // Tier 1: raw JSON string (no compile-time validation on Clang/C++17).
    BodyValue(string_view json)
        : str_(json), write_fn_(&write_string) {}

    BodyValue(const char* json)
        : str_(json), write_fn_(&write_string) {}
#endif

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

    // Tier 1: embed raw JSON string as body (no quoting).
    static void write_string(const void*, string_view s, JsonBuilder& b) {
        b.add_raw("body", s);
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

// ── Field-support traits (shared by ser, deser, template paths) ─────────────

// True for char[N] (distinguished from char pointers / std::array<char,N>).
template<typename T>
struct is_char_array : std::false_type {};

template<std::size_t N>
struct is_char_array<char[N]> : std::true_type {};

template<typename T>
constexpr bool is_char_array_v = is_char_array<T>::value;

// True for std::array<T, N>. Used to route fields into array ser/deser.
template<typename T>
struct is_std_array : std::false_type {};

template<typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template<typename T>
constexpr bool is_std_array_v = is_std_array<T>::value;

template<typename T>
struct array_traits;

template<typename T, std::size_t N>
struct array_traits<std::array<T, N>> {
    using element_type = T;
    static constexpr std::size_t size = N;
};

// True if T has a `.c_str()` method returning something pointer-like —
// detects Arduino String and similar buffer-owning string types for the
// ser path.
template<typename T, typename = void>
struct has_c_str : std::false_type {};

template<typename T>
struct has_c_str<T, std::void_t<decltype(std::declval<const T&>().c_str())>>
    : std::true_type {};

template<typename T>
constexpr bool has_c_str_v = has_c_str<T>::value;

// handler_accepts_v<H, F>: true if handler H claims field type F via its
// `handles_v<F>` trait, or true unconditionally when H doesn't expose
// that trait (e.g. generic lambda callers of _note_fields_dispatch).
// Lets filtered dispatch coexist with generic-lambda consumers.
template<typename Handler, typename F, typename = void>
struct handler_accepts : std::true_type {};

template<typename Handler, typename F>
struct handler_accepts<Handler, F,
    std::void_t<decltype(Handler::template handles_v<F>)>>
    : std::bool_constant<Handler::template handles_v<F>> {};

template<typename Handler, typename F>
constexpr bool handler_accepts_v = handler_accepts<Handler, F>::value;

#if __cplusplus >= 202002L

// C++20: detect aggregate types that can be reflected.
template<typename T>
concept ReflectableAggregate = std::is_aggregate_v<std::remove_cvref_t<T>>
    && !std::is_array_v<std::remove_cvref_t<T>>
    && !std::is_empty_v<std::remove_cvref_t<T>>
    && (reflect::size<std::remove_cvref_t<T>>() > 0);

#endif // C++20

// ── is_schema_struct ────────────────────────────────────────────────────────
// Unifies "struct with NOTE_FIELDS" and "C++20 reflectable aggregate".
// Used by write_field_value / write_template_hint to route nested-aggregate
// branches through a single trait.

template<typename T, typename = void>
struct is_schema_struct : has_note_fields_trait<T> {};

#if __cplusplus >= 202002L
template<typename T>
    requires (ReflectableAggregate<T> && !has_note_fields_trait<T>::value
              && !is_std_array_v<T> && !is_char_array_v<T>)
struct is_schema_struct<T, void> : std::true_type {};
#endif

template<typename T>
constexpr bool is_schema_struct_v = is_schema_struct<T>::value;

// ── write_field_value<V> ────────────────────────────────────────────────────
// Shared by write_field (C++20 reflected path) and _note_write_field
// (C++17 NOTE_FIELDS macro helper). One implementation of "write a struct
// field to a JsonBuilder under the given key".
//
// Supported types (symmetric with read_field_value and SaxAssign*):
//   bool, integral, floating-point, char[N], any type convertible to
//   string_view (std::string, const char*), any type with .c_str(),
//   any type convertible to const char*, nested schema struct,
//   std::array<Primitive, N> where Primitive is one of the above
//   primitive types.
//
// std::array<SchemaStruct, N>, std::array<std::array<...>, N>, and other
// nested-array / array-of-struct shapes are currently unsupported because
// JsonBuilder doesn't expose array-of-object primitives. static_assert
// catches these at compile time when NOTE_STRICT_BODY_FIELDS is enabled.

template<typename V>
void write_field_value(JsonBuilder& b, string_view name, const V& value);

// Write an array element (no key).
template<typename V>
void write_array_element(JsonBuilder& b, const V& value) {
    if constexpr (std::is_same_v<V, bool>) {
        b.add_element(value);
    } else if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
        b.add_element(static_cast<json_int_t>(value));
    } else if constexpr (std::is_floating_point_v<V>) {
        b.add_element(static_cast<double>(value));
    } else if constexpr (is_char_array_v<V>) {
        const char* p = value;
        size_t n = 0;
        while (n < sizeof(V) && p[n] != '\0') ++n;
        b.add_element(string_view(p, n));
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        b.add_element(string_view(value));
    } else if constexpr (has_c_str_v<V>) {
        b.add_element(string_view(value.c_str()));
    } else if constexpr (std::is_convertible_v<V, const char*>) {
        b.add_element(string_view(static_cast<const char*>(value)));
    } else {
#if NOTE_STRICT_BODY_FIELDS
        static_assert(sizeof(V) == 0,
            "note-cpp: std::array element type is not supported for "
            "serialisation. Arrays of nested schema structs or arrays "
            "of arrays are not currently supported on the ser path. "
            "Supported element types: bool, integral, floating-point, "
            "char[N], std::string-like, Arduino String. Use a lambda "
            "body for more complex shapes, or define "
            "NOTE_STRICT_BODY_FIELDS=0 to silently skip.");
#else
#pragma message("note-cpp: NOTE_STRICT_BODY_FIELDS=0 — array element type dropped silently.")
#endif
    }
}

// Forward decls for the generic path.
#if __cplusplus >= 202002L
template<typename T>
void write_aggregate(const T& obj, JsonBuilder& b);
#endif

template<typename V>
void write_field_value(JsonBuilder& b, string_view name, const V& value) {
    if constexpr (std::is_same_v<V, bool>) {
        b.add(name, value);
    } else if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
        b.add(name, static_cast<json_int_t>(value));
    } else if constexpr (std::is_floating_point_v<V>) {
        b.add(name, static_cast<double>(value));
    } else if constexpr (is_char_array_v<V>) {
        // Emit as string with length truncated at first NUL.
        const char* p = value;
        size_t n = 0;
        while (n < sizeof(V) && p[n] != '\0') ++n;
        b.add(name, string_view(p, n));
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        b.add(name, string_view(value));
    } else if constexpr (has_c_str_v<V>) {
        b.add(name, string_view(value.c_str()));
    } else if constexpr (std::is_convertible_v<V, const char*>) {
        b.add(name, string_view(static_cast<const char*>(value)));
    } else if constexpr (is_schema_struct_v<V>) {
        // Nested schema struct — recurse via NOTE_FIELDS or reflection.
        b.begin_object(name);
        if constexpr (has_note_fields_trait<V>::value) {
            V::_note_fields_write(value, b);
        }
#if __cplusplus >= 202002L
        else if constexpr (ReflectableAggregate<V>) {
            write_aggregate(value, b);
        }
#endif
        b.end_object();
    } else if constexpr (is_std_array_v<V>) {
        using Elem = typename array_traits<V>::element_type;
        b.begin_array(name);
        for (std::size_t i = 0; i < array_traits<V>::size; ++i) {
            write_array_element<Elem>(b, value[i]);
        }
        b.end_array();
    } else {
#if NOTE_STRICT_BODY_FIELDS
        static_assert(sizeof(V) == 0,
            "note-cpp: struct field type is not supported for "
            "serialisation. Supported: bool, integral, floating-point, "
            "char[N], string_view, std::string-like types, Arduino "
            "String (.c_str()), nested NOTE_FIELDS/aggregate structs, "
            "std::array<Primitive, N>. Define NOTE_STRICT_BODY_FIELDS=0 "
            "to silently skip unsupported fields.");
#else
#pragma message("note-cpp: NOTE_STRICT_BODY_FIELDS=0 — unsupported struct field type dropped silently.")
#endif
    }
}

#if __cplusplus >= 202002L

// Keep `write_field` as a thin alias for backwards compatibility with
// any external callers.
template<typename V>
void write_field(JsonBuilder& b, string_view name, const V& value) {
    write_field_value(b, name, value);
}

// Write all fields of an aggregate to the builder.
template<typename T>
void write_aggregate(const T& obj, JsonBuilder& b) {
    using R = std::remove_cvref_t<T>;
    [&]<std::size_t... Ns>(std::index_sequence<Ns...>) {
        (write_field_value(b, reflect::member_name<Ns, R>(), reflect::get<Ns>(obj)), ...);
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

// Per-V char-array template-hint string: N characters of 'x', one instance
// per N. Gives the Notecard a TSTRING(N) registration so runtime values
// up to N chars aren't truncated. Prior behaviour emitted "1" for every
// char array (forcing TSTRING(1)).
template<std::size_t N>
inline string_view char_array_template_filler() {
    static constexpr auto buf = []() {
        std::array<char, N> a{};
        for (std::size_t i = 0; i < N; ++i) a[i] = 'x';
        return a;
    }();
    return string_view(buf.data(), N);
}

#if __cplusplus >= 202002L
// Forward decl for C++20 reflective-aggregate hint iteration.
template<typename T>
void write_template_hints(JsonBuilder& b);
#endif

template<typename V>
void write_template_hint_for(JsonBuilder& b, string_view name);

// Backward-compat shim: the value is unused, only V's type matters.
template<typename V>
void write_template_hint(JsonBuilder& b, string_view name, const V& /*unused*/) {
    write_template_hint_for<V>(b, name);
}

template<typename V>
void write_template_hint_for(JsonBuilder& b, string_view name) {
    if constexpr (std::is_same_v<V, bool>) {
        b.add(name, true);
    } else if constexpr (std::is_floating_point_v<V>) {
        b.add(name, 14.1);
    } else if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
        // Use a representative value that hints at the storage size.
        if constexpr (sizeof(V) <= 1)
            b.add(name, json_int_t{1});        // TINT8
        else if constexpr (sizeof(V) <= 2)
            b.add(name, json_int_t{11});       // TINT16
        else
            b.add(name, json_int_t{12});       // TINT32
    } else if constexpr (is_char_array_v<V>) {
        // TSTRING(N): emit an N-char string so the Notecard registers
        // the correct maximum length for the field.
        b.add(name, char_array_template_filler<sizeof(V)>());
    } else if constexpr (std::is_convertible_v<V, string_view>
                      || has_c_str_v<V>
                      || std::is_convertible_v<V, const char*>) {
        // Unbounded string-like types. No max length available at the
        // type level; default to TSTRING(1). Users needing a larger
        // registration should switch to char[N].
        b.add(name, string_view("1"));
    } else if constexpr (is_schema_struct_v<V>) {
        b.begin_object(name);
        if constexpr (has_note_fields_trait<V>::value) {
            V::template _note_fields_write_hints<V>(b);
        }
#if __cplusplus >= 202002L
        else if constexpr (ReflectableAggregate<V>) {
            write_template_hints<V>(b);
        }
#endif
        b.end_object();
    } else if constexpr (is_std_array_v<V>) {
        using Elem = typename array_traits<V>::element_type;
        if constexpr (std::is_same_v<Elem, bool>
                   || std::is_floating_point_v<Elem>
                   || (std::is_integral_v<Elem> && !std::is_same_v<Elem, bool>)
                   || is_char_array_v<Elem>
                   || std::is_convertible_v<Elem, string_view>
                   || has_c_str_v<Elem>
                   || std::is_convertible_v<Elem, const char*>) {
            // Notecard array template: single-element hint array
            // describes the per-element type.
            b.begin_array(name);
            if constexpr (std::is_same_v<Elem, bool>) {
                b.add_element(true);
            } else if constexpr (std::is_floating_point_v<Elem>) {
                b.add_element(14.1);
            } else if constexpr (std::is_integral_v<Elem>) {
                if constexpr (sizeof(Elem) <= 1) b.add_element(json_int_t{1});
                else if constexpr (sizeof(Elem) <= 2) b.add_element(json_int_t{11});
                else b.add_element(json_int_t{12});
            } else if constexpr (is_char_array_v<Elem>) {
                b.add_element(char_array_template_filler<sizeof(Elem)>());
            } else {
                b.add_element(string_view("1"));
            }
            b.end_array();
        } else {
#if NOTE_STRICT_BODY_FIELDS
            static_assert(sizeof(V) == 0,
                "note-cpp: std::array of nested schema struct is not "
                "currently supported in template_of<T>. Define "
                "NOTE_STRICT_BODY_FIELDS=0 to silently skip this field "
                "in the template.");
#else
#pragma message("note-cpp: NOTE_STRICT_BODY_FIELDS=0 — std::array-of-struct dropped silently from template.")
#endif
        }
    } else {
#if NOTE_STRICT_BODY_FIELDS
        static_assert(sizeof(V) == 0,
            "note-cpp: struct field type is not supported for template "
            "registration. Supported: bool, integral, floating-point, "
            "char[N], string_view, std::string-like types, Arduino "
            "String (.c_str()), nested NOTE_FIELDS/aggregate structs, "
            "std::array of primitives. Define NOTE_STRICT_BODY_FIELDS=0 "
            "to silently skip unsupported fields.");
#else
#pragma message("note-cpp: NOTE_STRICT_BODY_FIELDS=0 — unsupported field type dropped silently from template.")
#endif
    }
}

#if __cplusplus >= 202002L
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
        +[](const void* ctx, string_view, JsonBuilder& b) {
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
    requires (detail::ReflectableAggregate<T> || detail::has_note_fields_trait<T>::value)
BodyValue template_of() {
    return BodyValue(
        nullptr,
        static_cast<BodyValue::WriteFn>([](const void*, string_view, JsonBuilder& b) {
            b.begin_object("body");
            if constexpr (detail::has_note_fields_trait<T>::value) {
                T::template _note_fields_write_hints<T>(b);
            } else {
                detail::write_template_hints<T>(b);
            }
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
    requires (detail::ReflectableAggregate<T> || detail::has_note_fields_trait<T>::value)
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

// Template-hint emitter: uses only the field's TYPE, not a value.
#define _NOTE_FIELDS_HINT_FIELD(b, field) \
    ::note::detail::write_template_hint_for< \
        ::std::remove_cv_t<decltype(Self_::field)>>(b, #field);

// Dispatch filters on Handler::handles_v<F> at compile time (via
// handler_accepts_v which defaults to true for handlers without the
// trait — preserves lambda-based callers of _note_fields_dispatch).
#define _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, field)                  \
    if constexpr (::note::detail::handler_accepts_v<                         \
            ::std::remove_cv_t<::std::remove_reference_t<decltype(handler)>>,\
            ::std::remove_cv_t<decltype((obj).field)>>) {                    \
        if ((k) == #field) { (handler)((obj).field); return true; }          \
    }

#define _NOTE_FIELDS_DESC_FIELD(field) \
    {#field, static_cast<uint16_t>(offsetof(Self_, field)), \
     ::note::field_type_of<decltype(Self_::field)>()},

#define NOTE_FIELDS(...) \
    template<typename Self_> \
    static void _note_fields_write(const Self_& _self, ::note::JsonBuilder& _b) { \
        _NOTE_FIELDS_EXPAND(_NOTE_FIELDS_WRITE_EACH(_self, _b, __VA_ARGS__)) \
    } \
    template<typename Self_> \
    static void _note_fields_read(Self_& _self, const ::note::JsonReader& _r) { \
        _NOTE_FIELDS_EXPAND(_NOTE_FIELDS_READ_EACH(_self, _r, __VA_ARGS__)) \
    } \
    template<typename Self_, typename Handler_> \
    static bool _note_fields_dispatch(Self_& _self, ::note::string_view _k, Handler_&& _handler) { \
        _NOTE_FIELDS_EXPAND(_NOTE_FIELDS_DISPATCH_EACH(_self, _k, _handler, __VA_ARGS__)) \
        return false; \
    } \
    template<typename Self_> \
    static void _note_fields_write_hints(::note::JsonBuilder& _b) { \
        _NOTE_FIELDS_EXPAND(_NOTE_FIELDS_HINT_EACH(_b, __VA_ARGS__)) \
    } \
    template<typename Self_> \
    static const ::note::FieldDesc* _note_field_descs(uint8_t& _n) { \
        static constexpr ::note::FieldDesc _table[] NOTE_FLASH_ATTR = { \
            _NOTE_FIELDS_EXPAND(_NOTE_FIELDS_DESC_EACH(__VA_ARGS__)) \
        }; \
        _n = sizeof(_table)/sizeof(_table[0]); \
        return _table; \
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

// Macro helpers for field iteration (dispatch path).
#define _NOTE_FIELDS_DISPATCH_EACH(obj, k, handler, ...) \
    _NOTE_FIELDS_DISPATCH_MAP(obj, k, handler, __VA_ARGS__)
#define _NOTE_FIELDS_DISPATCH_MAP(obj, k, handler, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_N(obj, k, handler, __VA_ARGS__, \
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define _NOTE_FIELDS_DISPATCH_MAP_N(obj, k, handler, \
    f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16, N, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_##N(obj, k, handler, \
        f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16)
#define _NOTE_FIELDS_DISPATCH_MAP_1(obj, k, handler, f1, ...) \
    _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f1)
#define _NOTE_FIELDS_DISPATCH_MAP_2(obj, k, handler, f1, f2, ...) \
    _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f1) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f2)
#define _NOTE_FIELDS_DISPATCH_MAP_3(obj, k, handler, f1, f2, f3, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_2(obj, k, handler, f1, f2) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f3)
#define _NOTE_FIELDS_DISPATCH_MAP_4(obj, k, handler, f1, f2, f3, f4, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_3(obj, k, handler, f1, f2, f3) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f4)
#define _NOTE_FIELDS_DISPATCH_MAP_5(obj, k, handler, f1, f2, f3, f4, f5, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_4(obj, k, handler, f1, f2, f3, f4) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f5)
#define _NOTE_FIELDS_DISPATCH_MAP_6(obj, k, handler, f1, f2, f3, f4, f5, f6, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_5(obj, k, handler, f1, f2, f3, f4, f5) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f6)
#define _NOTE_FIELDS_DISPATCH_MAP_7(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_6(obj, k, handler, f1, f2, f3, f4, f5, f6) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f7)
#define _NOTE_FIELDS_DISPATCH_MAP_8(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_7(obj, k, handler, f1, f2, f3, f4, f5, f6, f7) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f8)
#define _NOTE_FIELDS_DISPATCH_MAP_9(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_8(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f9)
#define _NOTE_FIELDS_DISPATCH_MAP_10(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_9(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f10)
#define _NOTE_FIELDS_DISPATCH_MAP_11(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_10(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f11)
#define _NOTE_FIELDS_DISPATCH_MAP_12(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_11(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f12)
#define _NOTE_FIELDS_DISPATCH_MAP_13(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_12(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f13)
#define _NOTE_FIELDS_DISPATCH_MAP_14(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_13(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f14)
#define _NOTE_FIELDS_DISPATCH_MAP_15(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_14(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f15)
#define _NOTE_FIELDS_DISPATCH_MAP_16(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, ...) \
    _NOTE_FIELDS_DISPATCH_MAP_15(obj, k, handler, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15) _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, f16)

// Macro helpers for field iteration (template-hint path).
#define _NOTE_FIELDS_HINT_EACH(b, ...) _NOTE_FIELDS_HINT_MAP(b, __VA_ARGS__)
#define _NOTE_FIELDS_HINT_MAP(b, ...) \
    _NOTE_FIELDS_HINT_MAP_N(b, __VA_ARGS__, \
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define _NOTE_FIELDS_HINT_MAP_N(b, \
    f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16, N, ...) \
    _NOTE_FIELDS_HINT_MAP_##N(b, \
        f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16)
#define _NOTE_FIELDS_HINT_MAP_1(b, f1, ...) \
    _NOTE_FIELDS_HINT_FIELD(b, f1)
#define _NOTE_FIELDS_HINT_MAP_2(b, f1, f2, ...) \
    _NOTE_FIELDS_HINT_FIELD(b, f1) _NOTE_FIELDS_HINT_FIELD(b, f2)
#define _NOTE_FIELDS_HINT_MAP_3(b, f1, f2, f3, ...) \
    _NOTE_FIELDS_HINT_MAP_2(b, f1, f2) _NOTE_FIELDS_HINT_FIELD(b, f3)
#define _NOTE_FIELDS_HINT_MAP_4(b, f1, f2, f3, f4, ...) \
    _NOTE_FIELDS_HINT_MAP_3(b, f1, f2, f3) _NOTE_FIELDS_HINT_FIELD(b, f4)
#define _NOTE_FIELDS_HINT_MAP_5(b, f1, f2, f3, f4, f5, ...) \
    _NOTE_FIELDS_HINT_MAP_4(b, f1, f2, f3, f4) _NOTE_FIELDS_HINT_FIELD(b, f5)
#define _NOTE_FIELDS_HINT_MAP_6(b, f1, f2, f3, f4, f5, f6, ...) \
    _NOTE_FIELDS_HINT_MAP_5(b, f1, f2, f3, f4, f5) _NOTE_FIELDS_HINT_FIELD(b, f6)
#define _NOTE_FIELDS_HINT_MAP_7(b, f1, f2, f3, f4, f5, f6, f7, ...) \
    _NOTE_FIELDS_HINT_MAP_6(b, f1, f2, f3, f4, f5, f6) _NOTE_FIELDS_HINT_FIELD(b, f7)
#define _NOTE_FIELDS_HINT_MAP_8(b, f1, f2, f3, f4, f5, f6, f7, f8, ...) \
    _NOTE_FIELDS_HINT_MAP_7(b, f1, f2, f3, f4, f5, f6, f7) _NOTE_FIELDS_HINT_FIELD(b, f8)
#define _NOTE_FIELDS_HINT_MAP_9(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, ...) \
    _NOTE_FIELDS_HINT_MAP_8(b, f1, f2, f3, f4, f5, f6, f7, f8) _NOTE_FIELDS_HINT_FIELD(b, f9)
#define _NOTE_FIELDS_HINT_MAP_10(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, ...) \
    _NOTE_FIELDS_HINT_MAP_9(b, f1, f2, f3, f4, f5, f6, f7, f8, f9) _NOTE_FIELDS_HINT_FIELD(b, f10)
#define _NOTE_FIELDS_HINT_MAP_11(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, ...) \
    _NOTE_FIELDS_HINT_MAP_10(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10) _NOTE_FIELDS_HINT_FIELD(b, f11)
#define _NOTE_FIELDS_HINT_MAP_12(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, ...) \
    _NOTE_FIELDS_HINT_MAP_11(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11) _NOTE_FIELDS_HINT_FIELD(b, f12)
#define _NOTE_FIELDS_HINT_MAP_13(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, ...) \
    _NOTE_FIELDS_HINT_MAP_12(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12) _NOTE_FIELDS_HINT_FIELD(b, f13)
#define _NOTE_FIELDS_HINT_MAP_14(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, ...) \
    _NOTE_FIELDS_HINT_MAP_13(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13) _NOTE_FIELDS_HINT_FIELD(b, f14)
#define _NOTE_FIELDS_HINT_MAP_15(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, ...) \
    _NOTE_FIELDS_HINT_MAP_14(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14) _NOTE_FIELDS_HINT_FIELD(b, f15)
#define _NOTE_FIELDS_HINT_MAP_16(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, ...) \
    _NOTE_FIELDS_HINT_MAP_15(b, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15) _NOTE_FIELDS_HINT_FIELD(b, f16)

// Macro helpers for field iteration (descriptor table path).
#define _NOTE_FIELDS_DESC_EACH(...) \
    _NOTE_FIELDS_DESC_MAP(__VA_ARGS__)
#define _NOTE_FIELDS_DESC_MAP(...) \
    _NOTE_FIELDS_DESC_MAP_N(__VA_ARGS__, \
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define _NOTE_FIELDS_DESC_MAP_N( \
    f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16, N, ...) \
    _NOTE_FIELDS_DESC_MAP_##N( \
        f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14,f15,f16)
#define _NOTE_FIELDS_DESC_MAP_1(f1, ...) \
    _NOTE_FIELDS_DESC_FIELD(f1)
#define _NOTE_FIELDS_DESC_MAP_2(f1, f2, ...) \
    _NOTE_FIELDS_DESC_FIELD(f1) _NOTE_FIELDS_DESC_FIELD(f2)
#define _NOTE_FIELDS_DESC_MAP_3(f1, f2, f3, ...) \
    _NOTE_FIELDS_DESC_MAP_2(f1, f2) _NOTE_FIELDS_DESC_FIELD(f3)
#define _NOTE_FIELDS_DESC_MAP_4(f1, f2, f3, f4, ...) \
    _NOTE_FIELDS_DESC_MAP_3(f1, f2, f3) _NOTE_FIELDS_DESC_FIELD(f4)
#define _NOTE_FIELDS_DESC_MAP_5(f1, f2, f3, f4, f5, ...) \
    _NOTE_FIELDS_DESC_MAP_4(f1, f2, f3, f4) _NOTE_FIELDS_DESC_FIELD(f5)
#define _NOTE_FIELDS_DESC_MAP_6(f1, f2, f3, f4, f5, f6, ...) \
    _NOTE_FIELDS_DESC_MAP_5(f1, f2, f3, f4, f5) _NOTE_FIELDS_DESC_FIELD(f6)
#define _NOTE_FIELDS_DESC_MAP_7(f1, f2, f3, f4, f5, f6, f7, ...) \
    _NOTE_FIELDS_DESC_MAP_6(f1, f2, f3, f4, f5, f6) _NOTE_FIELDS_DESC_FIELD(f7)
#define _NOTE_FIELDS_DESC_MAP_8(f1, f2, f3, f4, f5, f6, f7, f8, ...) \
    _NOTE_FIELDS_DESC_MAP_7(f1, f2, f3, f4, f5, f6, f7) _NOTE_FIELDS_DESC_FIELD(f8)
#define _NOTE_FIELDS_DESC_MAP_9(f1, f2, f3, f4, f5, f6, f7, f8, f9, ...) \
    _NOTE_FIELDS_DESC_MAP_8(f1, f2, f3, f4, f5, f6, f7, f8) _NOTE_FIELDS_DESC_FIELD(f9)
#define _NOTE_FIELDS_DESC_MAP_10(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, ...) \
    _NOTE_FIELDS_DESC_MAP_9(f1, f2, f3, f4, f5, f6, f7, f8, f9) _NOTE_FIELDS_DESC_FIELD(f10)
#define _NOTE_FIELDS_DESC_MAP_11(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, ...) \
    _NOTE_FIELDS_DESC_MAP_10(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10) _NOTE_FIELDS_DESC_FIELD(f11)
#define _NOTE_FIELDS_DESC_MAP_12(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, ...) \
    _NOTE_FIELDS_DESC_MAP_11(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11) _NOTE_FIELDS_DESC_FIELD(f12)
#define _NOTE_FIELDS_DESC_MAP_13(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, ...) \
    _NOTE_FIELDS_DESC_MAP_12(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12) _NOTE_FIELDS_DESC_FIELD(f13)
#define _NOTE_FIELDS_DESC_MAP_14(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, ...) \
    _NOTE_FIELDS_DESC_MAP_13(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13) _NOTE_FIELDS_DESC_FIELD(f14)
#define _NOTE_FIELDS_DESC_MAP_15(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, ...) \
    _NOTE_FIELDS_DESC_MAP_14(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14) _NOTE_FIELDS_DESC_FIELD(f15)
#define _NOTE_FIELDS_DESC_MAP_16(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, ...) \
    _NOTE_FIELDS_DESC_MAP_15(f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15) _NOTE_FIELDS_DESC_FIELD(f16)

namespace detail {

// Helper used by NOTE_FIELDS macro — delegates to write_field_value, which
// is shared with the C++20 reflected path. Keeps ser behaviour identical
// across both dialects.
template<typename V>
void _note_write_field(JsonBuilder& b, string_view name, const V& value) {
    write_field_value(b, name, value);
}

// ── read_field_value<V> ──────────────────────────────────────────────────
// Shared by _note_read_field (C++17 NOTE_FIELDS macro helper) and
// read_field (C++20 reflected path). Same supported-type coverage as
// write_field_value, so a schema struct round-trips symmetrically.
//
// Note: JsonReader only exposes get_string_array for arrays today; numeric
// and struct arrays are not currently supported on the random-access deser
// path. Users needing array fields should prefer streaming deser via
// .into(struct) on a request builder (StructSink) which handles all
// array shapes supported by the sink.

template<typename V>
void read_field_value(const JsonReader& r, string_view name, V& out);

// Forward decl — parse<T> is defined below but referenced recursively for
// nested-schema fields.
} // namespace detail

template<typename T,
    typename = std::enable_if_t<detail::has_note_fields_trait<T>::value>>
T parse(const JsonReader& r);

#if __cplusplus >= 202002L
template<typename T>
    requires (detail::ReflectableAggregate<T> && !detail::has_note_fields_trait<T>::value)
T parse(const JsonReader& r);
#endif

namespace detail {

template<typename V>
void read_field_value(const JsonReader& r, string_view name, V& out) {
    if constexpr (std::is_same_v<V, bool>) {
        out = r.get_bool(name);
    } else if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
        out = static_cast<V>(r.get_int(name));
    } else if constexpr (std::is_floating_point_v<V>) {
        out = static_cast<V>(r.get_double(name));
    } else if constexpr (is_char_array_v<V>) {
        // char[N]: copy string into the array with null terminator.
        string_view sv = r.get_string(name);
        constexpr std::size_t N = sizeof(V);
        std::size_t copy_len = sv.size() < N ? sv.size() : (N - 1);
        for (std::size_t i = 0; i < copy_len; ++i) out[i] = sv[i];
        out[copy_len] = '\0';
    } else if constexpr (std::is_same_v<V, string_view>) {
        out = r.get_string(name);
    } else if constexpr (std::is_constructible_v<V, string_view>) {
        out = V(r.get_string(name));
    } else if constexpr (std::is_constructible_v<V, const char*, size_t>) {
        string_view sv = r.get_string(name);
        out = V(sv.data(), sv.size());
    } else if constexpr (std::is_constructible_v<V, const char*>) {
        string_view sv = r.get_string(name);
        if (!sv.empty()) out = V(sv.data()); else out = V{};
    } else if constexpr (is_schema_struct_v<V>) {
        auto child = r.get_object(name);
        if (child) {
            out = ::note::parse<V>(*child);
        }
    } else if constexpr (is_std_array_v<V>) {
        using Elem = typename array_traits<V>::element_type;
        if constexpr (std::is_same_v<Elem, string_view>) {
            r.get_string_array(name, out.data(), out.size());
        } else {
#if NOTE_STRICT_BODY_FIELDS
            static_assert(sizeof(V) == 0,
                "note-cpp: std::array element type is not supported by "
                "random-access deser (parse<T>). JsonReader only exposes "
                "get_string_array today. Prefer streaming deser via "
                ".into(struct) on a request builder — it handles all "
                "array shapes. Define NOTE_STRICT_BODY_FIELDS=0 to "
                "silently skip.");
#else
#pragma message("note-cpp: NOTE_STRICT_BODY_FIELDS=0 — std::array field dropped silently from random-access deser.")
#endif
        }
    } else {
#if NOTE_STRICT_BODY_FIELDS
        static_assert(sizeof(V) == 0,
            "note-cpp: struct field type is not supported for "
            "random-access deser (parse<T>). Supported: bool, integral, "
            "floating-point, char[N], string_view, std::string-like types, "
            "Arduino String, nested NOTE_FIELDS/aggregate structs, "
            "std::array<string_view, N>. Define NOTE_STRICT_BODY_FIELDS=0 "
            "to silently skip unsupported fields.");
#else
#pragma message("note-cpp: NOTE_STRICT_BODY_FIELDS=0 — unsupported field type dropped silently from random-access deser.")
#endif
    }
}

// Helper used by NOTE_FIELDS macro — delegates to read_field_value.
template<typename V>
void _note_read_field(const JsonReader& r, string_view name, V& out) {
    read_field_value(r, name, out);
}

} // namespace detail


// ── Response body parsing ───────────────────────────────────────────────────
// Parse a JSON body object into a schema struct.
//
// Usage:
//   auto reader = result->body();  // get body JsonReader
//   auto readings = note::parse<Readings>(*reader);

// C++17: parse types registered with NOTE_FIELDS macro.
template<typename T, typename>
T parse(const JsonReader& r) {
    T obj{};
    T::_note_fields_read(obj, r);
    return obj;
}

#if __cplusplus >= 202002L

namespace detail {

template<typename V>
V read_field(const JsonReader& r, string_view name) {
    V out{};
    read_field_value(r, name, out);
    return out;
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
