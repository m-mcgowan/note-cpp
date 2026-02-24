#pragma once

#include <optional>
#include <type_traits>

namespace note {

/// Drop-in replacement for std::optional<T> with implicit read conversion.
/// Enables `T val = req.field;` instead of `T val = *req.field;`.
template<typename T>
struct Field : std::optional<T> {
    using std::optional<T>::optional;

    // Inherit copy/move constructors but provide unambiguous assignment.
    constexpr Field() = default;
    constexpr Field(const Field&) = default;
    constexpr Field(Field&&) = default;
    constexpr Field& operator=(const Field&) = default;
    constexpr Field& operator=(Field&&) = default;

    // Assignment from the underlying value type.
    template<typename U>
        requires std::is_convertible_v<U, T> && (!std::is_same_v<std::decay_t<U>, Field>)
    Field& operator=(U&& v) { std::optional<T>::operator=(T(std::forward<U>(v))); return *this; }

    Field& operator=(std::nullopt_t) { std::optional<T>::reset(); return *this; }

    /// Implicit conversion to const T& for ergonomic reads.
    /// Disabled for bool to avoid ambiguity with optional's explicit operator bool.
    operator const T&() const requires (!std::is_same_v<T, bool>) { return **this; }
};

} // namespace note
