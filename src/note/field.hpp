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

#ifndef NOTE_PRINTABLE
#ifdef ARDUINO
#define NOTE_PRINTABLE 1
#else
#define NOTE_PRINTABLE 0
#endif
#endif

#include <optional>
#include <type_traits>

namespace note {

namespace detail {

/// Shared printing logic for Arduino Printable support.
/// Extracted to avoid duplication between RequestField and ResponseField.
#if defined(ARDUINO) && NOTE_PRINTABLE
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

#if defined(ARDUINO) && NOTE_PRINTABLE
    size_t printTo(Print& p) const { return detail::print_field(p, *this); }
#endif
};


/// Read-only field for parsed responses.
///
/// With NOTE_RESPONSE_PRESENCE (default): extends std::optional<T> so callers
/// can distinguish "field absent" from "field was zero/empty".
///
/// Without NOTE_RESPONSE_PRESENCE: plain value wrapper, zero-initialized if
/// absent from the JSON response. Smaller but lossy for integer/bool fields.
#ifndef NOTE_RESPONSE_PRESENCE
#define NOTE_RESPONSE_PRESENCE 1
#endif

#if NOTE_RESPONSE_PRESENCE

template<typename T>
struct ResponseField
#if defined(ARDUINO) && NOTE_PRINTABLE
    : public Printable
#endif
{
    T value_{};
    bool present_{false};

    constexpr ResponseField() = default;
    constexpr ResponseField(const ResponseField&) = default;
    constexpr ResponseField(ResponseField&&) = default;
    constexpr ResponseField& operator=(const ResponseField&) = default;
    constexpr ResponseField& operator=(ResponseField&&) = default;

    // Assignment from value — used by parse code. Sets present flag.
    template<typename U, std::enable_if_t<
        std::is_convertible_v<U, T> && !std::is_same_v<std::decay_t<U>, ResponseField>, int> = 0>
    ResponseField& operator=(U&& v) { value_ = T(std::forward<U>(v)); present_ = true; return *this; }

    /// Whether this field was present in the JSON response.
    constexpr bool has_value() const { return present_; }

    /// Implicit conversion to const T& for ergonomic reads.
    operator const T&() const { return value_; }

    /// Arrow operator for calling methods on the underlying value.
    const T* operator->() const { return &value_; }

    /// Explicit value access.
    const T& value() const { return value_; }

    /// Comparison operators — compare the underlying value.
    friend bool operator==(const ResponseField& lhs, const ResponseField& rhs) { return lhs.value_ == rhs.value_; }
    friend bool operator!=(const ResponseField& lhs, const ResponseField& rhs) { return lhs.value_ != rhs.value_; }

    template<typename U>
    friend bool operator==(const ResponseField& lhs, const U& rhs) { return lhs.value_ == rhs; }
    template<typename U>
    friend bool operator!=(const ResponseField& lhs, const U& rhs) { return lhs.value_ != rhs; }
    template<typename U>
    friend bool operator==(const U& lhs, const ResponseField& rhs) { return lhs == rhs.value_; }
    template<typename U>
    friend bool operator!=(const U& lhs, const ResponseField& rhs) { return lhs != rhs.value_; }

    // Forward common string_view methods.
    template<typename U = T>
    auto size() const -> decltype(std::declval<const U&>().size()) { return value_.size(); }
    template<typename U = T>
    auto data() const -> decltype(std::declval<const U&>().data()) { return value_.data(); }
    template<typename U = T>
    auto empty() const -> decltype(std::declval<const U&>().empty()) { return value_.empty(); }
    template<typename U = T, typename... Args>
    auto find(Args&&... args) const -> decltype(std::declval<const U&>().find(std::forward<Args>(args)...)) {
        return value_.find(std::forward<Args>(args)...);
    }

#if defined(ARDUINO) && NOTE_PRINTABLE
    size_t printTo(Print& p) const override {
        if (!present_) return p.print("(unset)");
        if constexpr (std::is_same_v<T, bool>) {
            return p.print(value_ ? "true" : "false");
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return p.write(reinterpret_cast<const uint8_t*>(value_.data()), value_.size());
        } else if constexpr (std::is_integral_v<T>) {
            return p.print(static_cast<long>(value_));
        } else if constexpr (std::is_floating_point_v<T>) {
            return p.print(static_cast<double>(value_));
        } else {
            return 0;
        }
    }
#endif
};

#else // !NOTE_RESPONSE_PRESENCE — plain value wrapper, no presence tracking

template<typename T>
struct ResponseField
#if defined(ARDUINO) && NOTE_PRINTABLE
    : public Printable
#endif
{
    T value_{};

    constexpr ResponseField() = default;
    constexpr ResponseField(const ResponseField&) = default;
    constexpr ResponseField(ResponseField&&) = default;
    constexpr ResponseField& operator=(const ResponseField&) = default;
    constexpr ResponseField& operator=(ResponseField&&) = default;

    // Assignment from value — used by parse code.
    template<typename U, std::enable_if_t<
        std::is_convertible_v<U, T> && !std::is_same_v<std::decay_t<U>, ResponseField>, int> = 0>
    ResponseField& operator=(U&& v) { value_ = T(std::forward<U>(v)); return *this; }

    /// Implicit conversion to const T& for ergonomic reads.
    operator const T&() const { return value_; }

    /// Arrow operator for calling methods on the underlying value.
    const T* operator->() const { return &value_; }

    /// Explicit value access.
    const T& value() const { return value_; }

    /// Stub: always true (no presence tracking).
    constexpr bool has_value() const { return true; }

    /// Comparison operators — compare the underlying value.
    friend bool operator==(const ResponseField& lhs, const ResponseField& rhs) { return lhs.value_ == rhs.value_; }
    friend bool operator!=(const ResponseField& lhs, const ResponseField& rhs) { return lhs.value_ != rhs.value_; }

    template<typename U>
    friend bool operator==(const ResponseField& lhs, const U& rhs) { return lhs.value_ == rhs; }
    template<typename U>
    friend bool operator!=(const ResponseField& lhs, const U& rhs) { return lhs.value_ != rhs; }
    template<typename U>
    friend bool operator==(const U& lhs, const ResponseField& rhs) { return lhs == rhs.value_; }
    template<typename U>
    friend bool operator!=(const U& lhs, const ResponseField& rhs) { return lhs != rhs.value_; }

    // Forward common string_view methods.
    template<typename U = T>
    auto size() const -> decltype(std::declval<const U&>().size()) { return value_.size(); }
    template<typename U = T>
    auto data() const -> decltype(std::declval<const U&>().data()) { return value_.data(); }
    template<typename U = T>
    auto empty() const -> decltype(std::declval<const U&>().empty()) { return value_.empty(); }
    template<typename U = T, typename... Args>
    auto find(Args&&... args) const -> decltype(std::declval<const U&>().find(std::forward<Args>(args)...)) {
        return value_.find(std::forward<Args>(args)...);
    }

#if defined(ARDUINO) && NOTE_PRINTABLE
    size_t printTo(Print& p) const override {
        if constexpr (std::is_same_v<T, bool>) {
            return p.print(value_ ? "true" : "false");
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return p.write(reinterpret_cast<const uint8_t*>(value_.data()), value_.size());
        } else if constexpr (std::is_integral_v<T>) {
            return p.print(static_cast<long>(value_));
        } else if constexpr (std::is_floating_point_v<T>) {
            return p.print(static_cast<double>(value_));
        } else {
            return 0;
        }
    }
#endif
};

#endif // NOTE_RESPONSE_PRESENCE


/// Backward-compatible alias. Existing code using Field<T> continues to work.
template<typename T>
using Field = RequestField<T>;

} // namespace note
