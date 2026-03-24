// Minimal Print/Printable stubs for testing Arduino code paths without
// a real Arduino SDK. Include this before any note-cpp headers when
// compiling with -DARDUINO -DNOTE_ARDUINO_STUBS.
//
// In a real Arduino build, these types come from Arduino.h.

#pragma once

#ifdef NOTE_ARDUINO_STUBS

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class Print {
public:
    std::string buf;
    virtual ~Print() = default;
    virtual size_t write(const uint8_t* data, size_t len) {
        if (data && len > 0)
            buf.append(reinterpret_cast<const char*>(data), len);
        return len;
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
