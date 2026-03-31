#pragma once

#include <cstdint>
#include <cstdio>
#include <string_view>

#include "arduino/compat.hpp"

#include "compiler.hpp"

namespace note {

/// Phase-based error category — tells the caller what happened and whether
/// retrying is safe, not just what symptom was observed.
enum class Error : uint8_t {
    /// No error — default state.
    NoError,
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
    Error code{};
    Cause cause{};
    std::string_view message;

    ErrorInfo() = default;
    constexpr ErrorInfo(Error c, Cause ca, std::string_view m)
        : code(c), cause(ca), message(m) {}
    constexpr ErrorInfo(Error c, std::string_view m)
        : code(c), message(m) {}

#ifdef ARDUINO
    /// Arduino Printable support — allows Serial.println(result.error()).
    size_t printTo(Print& p) const override;
#endif
};

constexpr std::string_view to_string(Error e) {
    switch (e) {
    case Error::NoError:      return "no_error";
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

/// Fixed-size formatted error string — no heap allocation.
/// Format: "code[cause]: message" or "code: message" if cause is Unspecified.
/// Truncates if the message exceeds the buffer.
struct ErrorString {
    static constexpr size_t MAX_LEN = 255;
    char buf[MAX_LEN + 1]{};
    size_t len = 0;

    operator std::string_view() const { return {buf, len}; }
    bool operator==(std::string_view other) const { return std::string_view(*this) == other; }
    bool operator!=(std::string_view other) const { return std::string_view(*this) != other; }
    const char* c_str() const { return buf; }
    const char* data() const { return buf; }
    size_t size() const { return len; }
};

/// Format an ErrorInfo into a fixed buffer. No heap allocation.
inline ErrorString to_string(const ErrorInfo& e) {
    ErrorString out;
    auto code = to_string(e.code);
    auto cause = to_string(e.cause);

    // Write code
    size_t pos = 0;
    for (size_t i = 0; i < code.size() && pos < ErrorString::MAX_LEN; ++i)
        out.buf[pos++] = code[i];

    // Write [cause] if not Unspecified
    if (e.cause != Cause::Unspecified && pos < ErrorString::MAX_LEN) {
        out.buf[pos++] = '[';
        for (size_t i = 0; i < cause.size() && pos < ErrorString::MAX_LEN; ++i)
            out.buf[pos++] = cause[i];
        if (pos < ErrorString::MAX_LEN) out.buf[pos++] = ']';
    }

    // Write ": "
    if (pos < ErrorString::MAX_LEN) out.buf[pos++] = ':';
    if (pos < ErrorString::MAX_LEN) out.buf[pos++] = ' ';

    // Write message (truncate if needed)
    for (size_t i = 0; i < e.message.size() && pos < ErrorString::MAX_LEN; ++i)
        out.buf[pos++] = e.message[i];

    out.buf[pos] = '\0';
    out.len = pos;
    return out;
}

#ifdef ARDUINO
inline size_t ErrorInfo::printTo(Print& p) const {
    auto s = to_string(*this);
    return p.write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
#endif

} // namespace note
