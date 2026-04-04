// StructSink<T> — SAX-to-struct dispatcher for streaming body parse.
//
// Dispatches SAX events directly into struct fields using compile-time
// reflection (C++20) or NOTE_FIELDS macro (C++17). Primitives are
// assigned directly (zero arena cost). Strings are interned into the
// StringPool.
//
// Usage:
//   SensorData data{};
//   note::StructSink<SensorData> sink(data, pool);
//   sink.on_float("temperature", 22.5);   // data.temperature = 22.5f
//   sink.on_string("label", "room-42");   // data.label = pool.intern(...)
#pragma once

#include "body.hpp"
#include "json_sax.hpp"
#include "string_pool.hpp"
#include "types.hpp"

#include <cstddef>
#include <type_traits>

namespace note {

namespace detail {

// ── Type-dispatched SAX assignment helpers ──────────────────────────────
// Each handler is a callable struct that assigns a value to a field
// only if the field type matches. Uses if constexpr for zero overhead.

struct SaxAssignBool {
    bool value;
    template<typename F>
    void operator()(F& field) const {
        if constexpr (std::is_same_v<std::remove_cv_t<F>, bool>) {
            field = value;
        }
    }
};

struct SaxAssignInt {
    int32_t value;
    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
            field = static_cast<V>(value);
        } else if constexpr (std::is_floating_point_v<V>) {
            field = static_cast<V>(value);
        }
    }
};

struct SaxAssignFloat {
    double value;
    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        if constexpr (std::is_floating_point_v<V>) {
            field = static_cast<V>(value);
        } else if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
            field = static_cast<V>(value);
        }
    }
};

struct SaxAssignString {
    string_view value;
    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        if constexpr (std::is_same_v<V, string_view> || std::is_convertible_v<string_view, V>) {
            field = V(value);
        }
    }
};

struct SaxAssignNumber {
    string_view raw;
    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        if constexpr (std::is_floating_point_v<V>) {
            field = static_cast<V>(parse_double(raw));
        } else if constexpr (std::is_integral_v<V> && !std::is_same_v<V, bool>) {
            field = static_cast<V>(parse_int(raw));
        }
    }
};

// ── Dispatch: route SAX event to the right field ───────────────────────

// C++17: dispatch via NOTE_FIELDS _note_fields_dispatch.
template<typename T, typename Handler,
    std::enable_if_t<has_note_fields_trait<T>::value, int> = 0>
bool sax_dispatch(T& obj, string_view key, Handler&& handler) {
    return T::_note_fields_dispatch(obj, key, std::forward<Handler>(handler));
}

#if __cplusplus >= 202002L
// C++20: dispatch via aggregate reflection.
template<typename T, typename Handler>
    requires (ReflectableAggregate<T> && !has_note_fields_trait<T>::value)
bool sax_dispatch(T& obj, string_view key, Handler&& handler) {
    using R = std::remove_cvref_t<T>;
    bool matched = false;
    [&]<std::size_t... Ns>(std::index_sequence<Ns...>) {
        ((key == reflect::member_name<Ns, R>() &&
          (handler(reflect::get<Ns>(obj)), matched = true, true)), ...);
    }(std::make_index_sequence<reflect::size<R>()>{});
    return matched;
}
#endif

} // namespace detail

/// SAX-to-struct dispatcher. Receives SAX events and writes matching
/// fields directly into the target struct. Unmatched fields are ignored.
template<typename T>
struct StructSink {
    T& obj;
    StringPool& pool;

    StructSink(T& obj_, StringPool& pool_) : obj(obj_), pool(pool_) {}

    void on_bool(string_view k, bool v) {
        detail::sax_dispatch(obj, k, detail::SaxAssignBool{v});
    }

    void on_int(string_view k, int32_t v) {
        detail::sax_dispatch(obj, k, detail::SaxAssignInt{v});
    }

    void on_float(string_view k, double v) {
        detail::sax_dispatch(obj, k, detail::SaxAssignFloat{v});
    }

    void on_string(string_view k, string_view v) {
        v = pool.intern(v);
        detail::sax_dispatch(obj, k, detail::SaxAssignString{v});
    }

    void on_number(string_view k, string_view raw) {
        detail::sax_dispatch(obj, k, detail::SaxAssignNumber{raw});
    }

    void on_null(string_view) {}
    void on_object_begin(string_view) {}
    void on_object_end(string_view) {}
    void on_array_begin(string_view) {}
    void on_array_end(string_view) {}
    void reset() { obj = T{}; }
};

} // namespace note
