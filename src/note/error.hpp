#pragma once
#include "note_config.hpp"

/// NOTE_PRINTABLE: set to 0 to disable Arduino Printable support on
/// ErrorInfo and ResponseField. Saves vtable entries and ~900 bytes flash.
/// Default: enabled when ARDUINO is defined.
#ifndef NOTE_PRINTABLE
#ifdef ARDUINO
#define NOTE_PRINTABLE 1
#else
#define NOTE_PRINTABLE 0
#endif
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "arduino/compat.hpp"
#include "progmem.hpp"

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

/// Error message carrier — a tagged pointer that holds either a RAM
/// string_view or a FlashString (program memory on AVR). All byte-level
/// access routes through accessors that do the right thing for the
/// active storage; on non-Harvard platforms the tag is effectively a
/// no-op and reads collapse to a plain load.
///
/// Implicit conversion to `std::string_view` is only enabled on
/// non-Harvard targets where flash and RAM share the address space.
/// On AVR callers must use `printTo`, indexing, or `operator==` — see
/// docs/internal/avr-flash-strings.md.
class ErrorMessage {
public:
    constexpr ErrorMessage() = default;
    constexpr ErrorMessage(std::string_view sv)
        : ptr_(sv.data()), len_(sv.size()), in_flash_(false) {}
    // Use strlen directly — `std::string_view(s).size()` compiled to
    // a fatter sequence on AVR than a plain strlen. always_inline so
    // the tiny body is emitted at each call site rather than called
    // out-of-line.
    //
    // NOTE_COVERAGE_OMIT on the mem-init-list: the always_inline ctor
    // emits a branch record per call site and lcov cannot merge them,
    // producing thousands of phantom "uncovered" branches for a trivial
    // null → 0 guard. The runtime behaviour is exercised elsewhere.
    __attribute__((always_inline)) inline
    ErrorMessage(const char* s)
        : ptr_(s), len_(s ? strlen(s) : 0), in_flash_(false) {}  // NOTE_COVERAGE_OMIT

    /// Construct from a FlashString (PROGMEM on AVR).
    constexpr ErrorMessage(FlashString fs)
        : ptr_(fs.ptr), len_(fs.len), in_flash_(true) {}

#ifdef ARDUINO
    /// Construct from an Arduino `F("...")` expression. Always_inline
    /// to match the scan-key path (without it the ctor is emitted
    /// out-of-line and adds flash bloat per call site).
    __attribute__((always_inline)) inline
    ErrorMessage(const __FlashStringHelper* f)
        : ptr_(reinterpret_cast<const char*>(f)),
#if NOTE_PROGMEM
          len_(strlen_P(reinterpret_cast<const char*>(f))),
#else
          len_(strlen(reinterpret_cast<const char*>(f))),
#endif
          in_flash_(true) {}
#endif

    constexpr size_t size() const { return len_; }
    constexpr bool empty() const { return len_ == 0; }
    constexpr bool is_flash() const { return in_flash_; }

    /// Byte access that handles both storages. On AVR with
    /// in_flash_=true this issues an LPM read; otherwise plain load.
    char operator[](size_t i) const {
#if NOTE_PROGMEM
        if (in_flash_) return static_cast<char>(pgm_read_byte(ptr_ + i));
#endif
        return ptr_[i];
    }

    /// Raw pointer — valid for direct dereference only when storage is
    /// RAM-backed, or on non-Harvard platforms. Provided for Phase 1
    /// compatibility; prefer operator[] or printTo on AVR.
    constexpr const char* data() const { return ptr_; }

    bool operator==(std::string_view other) const {
        if (other.size() != len_) return false;
#if NOTE_PROGMEM
        if (in_flash_) {
            for (size_t i = 0; i < len_; ++i) {
                if (other[i] != static_cast<char>(pgm_read_byte(ptr_ + i)))
                    return false;
            }
            return true;
        }
#endif
        for (size_t i = 0; i < len_; ++i) if (other[i] != ptr_[i]) return false;
        return true;
    }
    bool operator!=(std::string_view other) const { return !(*this == other); }

#if !NOTE_PROGMEM
    /// Implicit conversion to string_view — only enabled on non-Harvard
    /// targets where flash pointers are usable like RAM pointers.
    constexpr operator std::string_view() const { return {ptr_, len_}; }
#endif

#if defined(ARDUINO)
    /// Write the message bytes to a Print sink. Handles flash-backed
    /// messages byte-by-byte via pgm_read_byte on AVR.
    inline size_t printTo(Print& p) const {
#if NOTE_PROGMEM
        if (in_flash_) {
            size_t n = 0;
            for (size_t i = 0; i < len_; ++i) {
                uint8_t b = pgm_read_byte(ptr_ + i);
                n += p.write(b);
            }
            return n;
        }
#endif
        return p.write(reinterpret_cast<const uint8_t*>(ptr_), len_);
    }
#endif

private:
    const char* ptr_ = nullptr;
    size_t len_ = 0;
    bool in_flash_ = false;
};

struct ErrorInfo
#if defined(ARDUINO) && NOTE_PRINTABLE
    : public Printable
#endif
{
    Error code{};
    Cause cause{};
    ErrorMessage message;

    ErrorInfo() = default;
    constexpr ErrorInfo(Error c, Cause ca, ErrorMessage m)
        : code(c), cause(ca), message(m) {}
    constexpr ErrorInfo(Error c, ErrorMessage m)
        : code(c), message(m) {}

#if defined(ARDUINO) && NOTE_PRINTABLE
    /// Arduino Printable support — allows Serial.println(result.error()).
    size_t printTo(Print& p) const override;
#endif
};

/// Error name as a FlashString (PROGMEM on AVR — zero RAM cost for
/// the literals). Returned by the `to_string(Error)` overload below
/// so both the new flash-aware and legacy string_view-comparing
/// callers work without changes — FlashString has `operator==`
/// against `string_view`.
inline FlashString to_string(Error e) {
    static const char s_no_error[]        NOTE_FLASH_ATTR = "no_error";
    static const char s_send_failed[]     NOTE_FLASH_ATTR = "send_failed";
    static const char s_response_lost[]   NOTE_FLASH_ATTR = "response_lost";
    static const char s_notecard[]        NOTE_FLASH_ATTR = "notecard";
    static const char s_json[]            NOTE_FLASH_ATTR = "json";
    static const char s_not_ready[]       NOTE_FLASH_ATTR = "not_ready";
    static const char s_overflow[]        NOTE_FLASH_ATTR = "overflow";
    static const char s_invalid_arg[]     NOTE_FLASH_ATTR = "invalid_argument";
    switch (e) {
    case Error::NoError:      return {s_no_error,      sizeof(s_no_error)      - 1};
    case Error::SendFailed:   return {s_send_failed,   sizeof(s_send_failed)   - 1};
    case Error::ResponseLost: return {s_response_lost, sizeof(s_response_lost) - 1};
    case Error::Notecard:     return {s_notecard,      sizeof(s_notecard)      - 1};
    case Error::Json:         return {s_json,          sizeof(s_json)          - 1};
    case Error::NotReady:     return {s_not_ready,     sizeof(s_not_ready)     - 1};
    case Error::Overflow:     return {s_overflow,      sizeof(s_overflow)      - 1};
    case Error::InvalidArg:   return {s_invalid_arg,   sizeof(s_invalid_arg)   - 1};
    }
    NOTE_UNREACHABLE();
}

inline FlashString to_string(Cause c) {
    static const char s_unspecified[]     NOTE_FLASH_ATTR = "unspecified";
    static const char s_timeout[]         NOTE_FLASH_ATTR = "timeout";
    static const char s_timeout_intra[]   NOTE_FLASH_ATTR = "timeout_intra";
    static const char s_hal_error[]       NOTE_FLASH_ATTR = "hal_error";
    static const char s_crc_mismatch[]    NOTE_FLASH_ATTR = "crc_mismatch";
    switch (c) {
    case Cause::Unspecified:  return {s_unspecified,   sizeof(s_unspecified)   - 1};
    case Cause::Timeout:      return {s_timeout,       sizeof(s_timeout)       - 1};
    case Cause::TimeoutIntra: return {s_timeout_intra, sizeof(s_timeout_intra) - 1};
    case Cause::HalError:     return {s_hal_error,     sizeof(s_hal_error)     - 1};
    case Cause::CrcMismatch:  return {s_crc_mismatch,  sizeof(s_crc_mismatch)  - 1};
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

#if defined(ARDUINO) && NOTE_PRINTABLE
inline size_t ErrorInfo::printTo(Print& p) const {
    auto s = to_string(*this);
    return p.write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
#endif

} // namespace note
