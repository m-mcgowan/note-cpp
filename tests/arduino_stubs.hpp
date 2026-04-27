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

// ─── Wire stub (TwoWire) ─────────────────────────────────────────────────
// Minimal mock of Arduino's Wire library, just enough to compile and
// instrument note::arduino::I2CHal in host-side tests.
//
// Records every begin()/end() call so tests can assert who managed the
// bus. transmit/receive helpers are stubbed to make the SoI2C protocol
// path compile (they don't simulate real I2C).

#include <vector>

class TwoWire {
public:
    struct BeginCall { int sda; int scl; };

    std::vector<BeginCall> begin_calls;
    int end_count = 0;
    int set_buffer_size_arg = 0;

    // begin() with no args.
    void begin() { begin_calls.push_back({-1, -1}); }
    // begin(sda, scl).
    void begin(int sda, int scl) { begin_calls.push_back({sda, scl}); }
    void end() { ++end_count; }

    void setBufferSize(int n) { set_buffer_size_arg = n; }

    // SoI2C protocol surface — these no-ops let I2CHal::transmit /
    // I2CHal::receive compile in host stub builds.
    void beginTransmission(uint8_t /*addr*/) {}
    int  endTransmission() { return 0; }
    size_t write(uint8_t /*b*/) { return 1; }
    size_t write(const uint8_t* /*data*/, size_t len) { return len; }
    int  requestFrom(int /*addr*/, int qty) { return qty; }
    int  read() { return 0; }
    int  available() { return 0; }
};

// Stock Arduino doesn't auto-create a global Wire here; tests that
// need one can declare their own instance.

// Wire library has end() on most modern cores.
#ifndef WIRE_HAS_END
#define WIRE_HAS_END 1
#endif

// Arduino timing — global functions used by note::arduino::I2CHal etc.
inline uint32_t millis() { return 0; }
inline void delay(uint32_t /*ms*/) {}

#endif // NOTE_ARDUINO_STUBS
