#pragma once

#include <cstdint>
#include <expected>    // C++23: std::expected
#include <functional>
#include <memory>
#include <string_view>

#include "error.hpp"

namespace note {

using string_view = std::string_view;

// Opaque handle to a JSON object managed by a JsonBackend.
// The concrete type depends on the backend (e.g. cJSON's J*, nlohmann::json*).
using json_handle = void*;

// C++23: std::expected — retrofit to tl::expected for C++17
template<typename T>
using Result = std::expected<T, ErrorInfo>;

// C++23: std::unexpected — retrofit to tl::unexpected for C++17
using Unexpected = std::unexpected<ErrorInfo>;

inline Unexpected make_error(Error code, string_view message = {}) {
    if (message.empty()) message = to_string(code);
    return Unexpected(ErrorInfo{code, message});
}

} // namespace note
