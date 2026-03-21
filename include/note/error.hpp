#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "compiler.hpp"

namespace note {

/// Phase-based error category — tells the caller what happened and whether
/// retrying is safe, not just what symptom was observed.
enum class Error : uint8_t {
    /// Request never reached the Notecard — always safe to retry.
    SendFailed,
    /// Notecard may have processed the request but we couldn't read the
    /// response (timeout, CRC mismatch, I2C read failure, etc.).
    /// Retry only if is_safe_to_retry(request.safety).
    ResponseLost,
    /// Notecard returned a valid response with an {"err":"..."} field.
    Notecard,
    /// Local JSON build or parse failure.
    Json,
    /// HAL reset failed — transport not usable.
    NotReady,
    /// Buffer too small.
    Overflow,
    /// Bad caller input.
    InvalidArg,
};

/// Diagnostic detail — why the error happened. Callers that only need
/// retry decisions can ignore this; callers that want adaptive behavior
/// (back-off on repeated timeouts, bus reset on NACK) can switch on it.
enum class Cause : uint8_t {
    Unspecified,
    /// No response within the request deadline.
    Timeout,
    /// Response started arriving but stalled (intra-byte timeout).
    TimeoutIntra,
    /// I2C NACK or HAL transmit/receive returned false.
    HalError,
    /// Response CRC didn't match.
    CrcMismatch,
};

struct ErrorInfo
#ifdef ARDUINO
    : public Printable
#endif
{
    Error code;
    Cause cause{};
    std::string_view message;

#ifdef ARDUINO
    /// Arduino Printable support — allows Serial.println(result.error()).
    size_t printTo(Print& p) const override;
#endif
};

constexpr std::string_view to_string(Error e) {
    switch (e) {
    case Error::SendFailed:   return "send_failed";
    case Error::ResponseLost: return "response_lost";
    case Error::Notecard:     return "notecard";
    case Error::Json:         return "json";
    case Error::NotReady:     return "not_ready";
    case Error::Overflow:     return "overflow";
    case Error::InvalidArg:   return "invalid_argument";
    }
    NOTE_UNREACHABLE();
}

constexpr std::string_view to_string(Cause c) {
    switch (c) {
    case Cause::Unspecified:  return "unspecified";
    case Cause::Timeout:      return "timeout";
    case Cause::TimeoutIntra: return "timeout_intra";
    case Cause::HalError:     return "hal_error";
    case Cause::CrcMismatch:  return "crc_mismatch";
    }
    NOTE_UNREACHABLE();
}

/// Format an ErrorInfo for logging.
///   Cause Unspecified: "notecard: {some device has no ProductUID configured}"
///   Cause set:         "response_lost[timeout]: no response"
inline std::string to_string(const ErrorInfo& e) {
    std::string s;
    auto code = to_string(e.code);
    s.reserve(code.size() + 20 + e.message.size());
    s += code;
    if (e.cause != Cause::Unspecified) {
        s += '[';
        s += to_string(e.cause);
        s += ']';
    }
    s += ": ";
    s += e.message;
    return s;
}

#ifdef ARDUINO
inline size_t ErrorInfo::printTo(Print& p) const {
    std::string s = to_string(*this);
    return p.print(s.c_str());
}
#endif

} // namespace note
