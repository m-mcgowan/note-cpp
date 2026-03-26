#pragma once
/// @file binary_request.hpp
/// Mixin for request types that transfer binary data.
///
/// Provides .data() for send requests and .into() for receive requests,
/// attaching a buffer to the request. The regular execute() method detects
/// the attached buffer and handles COBS encoding/decoding transparently.

#include "span.hpp"

#include <type_traits>
#include <utility>

namespace note {

/// Mixin for binary send requests (card.binary.put).
/// Attach source data with .data(), then call .execute() as usual.
/// Post-transmit verification is on by default — the Notecard's stored
/// MD5 is checked after streaming to confirm it accepted the data.
struct BinarySendMixin {
    const_byte_span binary_src_{};
    bool binary_verify_ = true;

    bool has_binary_data() const { return binary_src_.data() != nullptr; }
};

/// Mixin for binary receive requests (card.binary.get, dfu.get).
/// Attach destination buffer with .into(), then call .execute() as usual.
struct BinaryReceiveMixin {
    byte_span binary_dst_{};

    bool has_binary_buffer() const { return binary_dst_.data() != nullptr; }
};

namespace detail {
    template<typename T, typename = void>
    struct has_binary_src : std::false_type {};
    template<typename T>
    struct has_binary_src<T, std::void_t<decltype(std::declval<T>().binary_src_)>> : std::true_type {};

    template<typename T, typename = void>
    struct has_binary_dst : std::false_type {};
    template<typename T>
    struct has_binary_dst<T, std::void_t<decltype(std::declval<T>().binary_dst_)>> : std::true_type {};
}

} // namespace note
