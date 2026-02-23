#pragma once

#include <cstdint>
#include <string_view>

namespace note {

enum class Error : uint8_t {
    Timeout,
    Transport,
    Json,
    Protocol,
    NotReady,
    Overflow,
    InvalidArg,
};

struct ErrorInfo {
    Error code;
    std::string_view message;
};

constexpr std::string_view to_string(Error e) {
    switch (e) {
    case Error::Timeout:    return "timeout";
    case Error::Transport:  return "transport";
    case Error::Json:       return "json";
    case Error::Protocol:   return "protocol";
    case Error::NotReady:   return "not ready";
    case Error::Overflow:   return "overflow";
    case Error::InvalidArg: return "invalid argument";
    }
    return "unknown"; // C++23: std::unreachable()
}

} // namespace note
