// Minimal Print/Printable stubs for testing Arduino code paths without
// a real Arduino SDK. Include this before any note-cpp headers when
// compiling with -DARDUINO -DNOTE_ARDUINO_STUBS.
//
// In a real Arduino build, these types come from Arduino.h.

#pragma once

#if NOTE_ARDUINO_STUBS

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Forward-declared marker type; never defined. Used only via
// `const __FlashStringHelper*` pointers — the actual bytes are
// reinterpreted as `const char*` by the code that reads them.
class __FlashStringHelper;

/// Arduino `F(...)` macro stub. On a real AVR toolchain this would
/// place the literal in PROGMEM; the host stub reinterprets the RAM
/// pointer so code that uses `F()` still compiles and runs.
#define F(str_literal) (reinterpret_cast<const __FlashStringHelper*>(str_literal))

class Print {
public:
    std::string buf;
    virtual ~Print() = default;
    virtual size_t write(const uint8_t* data, size_t len) {
        if (data && len > 0)
            buf.append(reinterpret_cast<const char*>(data), len);
        return len;
    }
    /// Single-byte write — used by ErrorMessage::printTo on AVR where
    /// bytes are emitted one at a time via pgm_read_byte.
    virtual size_t write(uint8_t b) {
        buf.push_back(static_cast<char>(b));
        return 1;
    }
    size_t print(const char* s) { buf += s; return std::strlen(s); }
    size_t print(long v) { auto s = std::to_string(v); buf += s; return s.size(); }
    size_t print(double v) { auto s = std::to_string(v); buf += s; return s.size(); }
    size_t println() { buf += '\n'; return 1; }
};

class Printable {
public:
    virtual ~Printable() = default;
    virtual size_t printTo(Print& p) const = 0;
};

#endif // NOTE_ARDUINO_STUBS
