#pragma once
/// @file field.hpp
/// Field types for request and response structs.
///
/// RequestField<T> — mutable optional field for request builders.
///   Unset fields are omitted from the JSON request.
///
/// ResponseField<T> — read-only optional field for parsed responses.
///   Unset means the field was not present in the JSON response.
///
/// Both support implicit conversion to const T& for ergonomic reads,
/// and Printable on Arduino for direct Serial.println() usage.

#include <optional>
#include <type_traits>

namespace note {

namespace detail {

/// Shared printing logic for Arduino Printable support.
/// Extracted to avoid duplication between RequestField and ResponseField.
#ifdef ARDUINO
template<typename T>
inline size_t print_field(Print& p, const std::optional<T>& opt) {
    if (!opt.has_value()) return p.print("(unset)");
    if constexpr (std::is_same_v<T, bool>) {
        return p.print(*opt ? "true" : "false");
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        return p.write(reinterpret_cast<const uint8_t*>((*opt).data()), (*opt).size());
    } else if constexpr (std::is_integral_v<T>) {
        return p.print(static_cast<long>(*opt));
    } else if constexpr (std::is_floating_point_v<T>) {
        return p.print(static_cast<double>(*opt));
    } else {
        return 0;
    }
}
#endif

} // namespace detail


/// Mutable optional field for request builders.
/// Unset fields (has_value() == false) are omitted from the JSON request.
template<typename T>
struct RequestField : std::optional<T> {
    using std::optional<T>::optional;

    constexpr RequestField() = default;
    constexpr RequestField(const RequestField&) = default;
    constexpr RequestField(RequestField&&) = default;
    constexpr RequestField& operator=(const RequestField&) = default;
    constexpr RequestField& operator=(RequestField&&) = default;

    // Assignment from the underlying value type.
#if __cplusplus >= 202002L
    template<typename U>
        requires std::is_convertible_v<U, T> && (!std::is_same_v<std::decay_t<U>, RequestField>)
    RequestField& operator=(U&& v) { std::optional<T>::operator=(T(std::forward<U>(v))); return *this; }
#else
    template<typename U, std::enable_if_t<
        std::is_convertible_v<U, T> && !std::is_same_v<std::decay_t<U>, RequestField>, int> = 0>
    RequestField& operator=(U&& v) { std::optional<T>::operator=(T(std::forward<U>(v))); return *this; }
#endif

    RequestField& operator=(std::nullopt_t) { std::optional<T>::reset(); return *this; }

    /// Implicit conversion to const T& for ergonomic reads.
    /// Disabled for bool to avoid ambiguity with optional's explicit operator bool.
#if __cplusplus >= 202002L
    operator const T&() const requires (!std::is_same_v<T, bool>) { return **this; }
#else
    template<typename U = T, std::enable_if_t<!std::is_same_v<U, bool>, int> = 0>
    operator const T&() const { return **this; }
#endif

#ifdef ARDUINO
    size_t printTo(Print& p) const { return detail::print_field(p, *this); }
#endif
};


/// Read-only optional field for parsed responses.
/// Unset (has_value() == false) means the field was not present in the JSON response.
/// Populated during parse, then immutable.
template<typename T>
struct ResponseField : std::optional<T> {
    using std::optional<T>::optional;

    constexpr ResponseField() = default;
    constexpr ResponseField(const ResponseField&) = default;
    constexpr ResponseField(ResponseField&&) = default;
    constexpr ResponseField& operator=(const ResponseField&) = default;
    constexpr ResponseField& operator=(ResponseField&&) = default;

    // Construction/assignment from value — used by parse code.
#if __cplusplus >= 202002L
    template<typename U>
        requires std::is_convertible_v<U, T> && (!std::is_same_v<std::decay_t<U>, ResponseField>)
    ResponseField& operator=(U&& v) { std::optional<T>::operator=(T(std::forward<U>(v))); return *this; }
#else
    template<typename U, std::enable_if_t<
        std::is_convertible_v<U, T> && !std::is_same_v<std::decay_t<U>, ResponseField>, int> = 0>
    ResponseField& operator=(U&& v) { std::optional<T>::operator=(T(std::forward<U>(v))); return *this; }
#endif

    /// Implicit conversion to const T& for ergonomic reads.
    /// Unlike RequestField, this works for ALL types including bool —
    /// ResponseField is always-populated in normal use, so the ambiguity
    /// with optional's operator bool is less of a concern. Users check
    /// has_value() explicitly when they need to know if the field was
    /// present in the response.
    operator const T&() const { return **this; }

    /// Arrow operator for calling methods on the underlying value.
    /// Enables: rsp.version->find("foo") instead of (*rsp.version).find("foo")
    const T* operator->() const { return &(**this); }

    /// Explicit value access — useful when implicit conversion can't chain
    /// (e.g. returning ResponseField<bool> where Result<bool> is expected).
    const T& value() const { return **this; }

    // Forward common string_view methods so rsp.field.size() / .data() /
    // .empty() / .find() work without dereferencing.
    template<typename U = T>
    auto size() const -> decltype(std::declval<const U&>().size()) {
        return (**this).size();
    }
    template<typename U = T>
    auto data() const -> decltype(std::declval<const U&>().data()) {
        return (**this).data();
    }
    template<typename U = T>
    auto empty() const -> decltype(std::declval<const U&>().empty()) {
        return (**this).empty();
    }
    template<typename U = T, typename... Args>
    auto find(Args&&... args) const -> decltype(std::declval<const U&>().find(std::forward<Args>(args)...)) {
        return (**this).find(std::forward<Args>(args)...);
    }

#ifdef ARDUINO
    size_t printTo(Print& p) const { return detail::print_field(p, *this); }
#endif
};


/// Backward-compatible alias. Existing code using Field<T> continues to work.
template<typename T>
using Field = RequestField<T>;

} // namespace note
