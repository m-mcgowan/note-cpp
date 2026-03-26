#pragma once

#include <cstddef>
#include <cstdint>

#if __has_include(<version>)
#  include <version>
#endif

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L

#include <span>
namespace note {
    template<typename T> using span = std::span<T>;
    using byte_span       = std::span<uint8_t>;
    using const_byte_span = std::span<const uint8_t>;
}

#else

namespace note {

/// Lightweight non-owning view over a contiguous sequence of T.
/// Drop-in substitute for std::span on C++17 targets.
template<typename T>
class span {
public:
    constexpr span() noexcept : data_(nullptr), size_(0) {}
    constexpr span(T* data, size_t size) noexcept : data_(data), size_(size) {}

    /// Implicit construction from a raw array — size deduced at compile time.
    template<size_t N>
    constexpr span(T (&arr)[N]) noexcept : data_(arr), size_(N) {}  // NOLINT(google-explicit-constructor)

    constexpr T*     data()  const noexcept { return data_; }
    constexpr size_t size()  const noexcept { return size_; }
    constexpr bool   empty() const noexcept { return size_ == 0; }

    constexpr T& operator[](size_t i) const noexcept { return data_[i]; }

private:
    T*     data_;
    size_t size_;
};

using byte_span       = span<uint8_t>;
using const_byte_span = span<const uint8_t>;

} // namespace note

#endif
