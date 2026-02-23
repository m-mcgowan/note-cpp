#pragma once

#include <cstdint>
#include <string_view>

namespace note {

enum class Safety : uint8_t {
    ReadOnly,
    Idempotent,
    NonIdempotent,
    Destructive,
};

constexpr std::string_view to_string(Safety s) {
    switch (s) {
    case Safety::ReadOnly:       return "readonly";
    case Safety::Idempotent:     return "idempotent";
    case Safety::NonIdempotent:  return "non-idempotent";
    case Safety::Destructive:    return "destructive";
    }
    return "unknown"; // C++23: std::unreachable()
}

constexpr bool is_safe_to_retry(Safety s) {
    return s == Safety::ReadOnly || s == Safety::Idempotent;
}

} // namespace note
