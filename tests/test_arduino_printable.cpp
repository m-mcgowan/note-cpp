// Tests for Arduino Printable support.
//
// Simulates an Arduino environment with minimal Print/Printable stubs
// to verify that generated printTo() methods compile and produce correct
// output for all field types including duration units.
//
// These tests catch issues that only appear under -DARDUINO, such as
// Seconds/Minutes fields being incorrectly treated as strings in printTo().

#include "catch.hpp"

// Minimal Arduino stubs — must be defined before ARDUINO is effective.
// In real Arduino, these come from Arduino.h.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

class Print {
public:
    std::string buf;
    virtual ~Print() = default;
    virtual size_t write(const uint8_t* data, size_t len) {
        if (data && len > 0)
            buf.append(reinterpret_cast<const char*>(data), len);
        return len;
    }
    size_t print(const char* s) { buf += s; return strlen(s); }
    size_t print(long v) { auto s = std::to_string(v); buf += s; return s.size(); }
    size_t print(double v) { auto s = std::to_string(v); buf += s; return s.size(); }
    size_t println() { buf += '\n'; return 1; }
};

class Printable {
public:
    virtual ~Printable() = default;
    virtual size_t printTo(Print& p) const = 0;
};

// Now enable Arduino code paths with stub implementations
#ifndef ARDUINO
#define ARDUINO
#endif
#define NOTE_ARDUINO_STUBS

// Include ALL generated headers to verify printTo compiles for every endpoint
#include <note/api.hpp>
#include <note/error.hpp>

// ---------------------------------------------------------------------------
// ErrorInfo Printable
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: ErrorInfo printTo") {
    Print p;
    note::ErrorInfo err{note::Error::Notecard, "device error"};
    err.printTo(p);
    REQUIRE(p.buf == "notecard: device error");
}

// ---------------------------------------------------------------------------
// Request printTo — including duration fields
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: HubSet request printTo") {
    Print p;
    note::api::HubSet req;
    req.product = "com.example.app";
    req.mode = "periodic";
    req.printTo(p);
    REQUIRE(p.buf.find("hub.set") != std::string::npos);
    REQUIRE(p.buf.find("com.example.app") != std::string::npos);
    REQUIRE(p.buf.find("periodic") != std::string::npos);
}

TEST_CASE("Arduino: CardAttn Arm with Seconds printTo") {
    Print p;
    note::api::CardAttn::Arm req;
    req.seconds = note::Seconds{120};
    req.printTo(p);
    // Should contain the seconds value as a number, not call .data()
    REQUIRE(p.buf.find("120") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Response printTo
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: HubSet with Minutes printTo") {
    Print p;
    note::api::HubSet req;
    req.outbound = note::Minutes{60};
    req.printTo(p);
    // Minutes should print as an integer (60), not call .data()
    REQUIRE(p.buf.find("60") != std::string::npos);
    REQUIRE(p.buf.find("outbound") != std::string::npos);
}

TEST_CASE("Arduino: print_json_value dispatch") {
    Print p;

    // bool
    p.buf.clear();
    note::detail::print_json_value(p, true);
    REQUIRE(p.buf == "true");

    // int
    p.buf.clear();
    note::detail::print_json_value(p, int32_t{42});
    REQUIRE(p.buf == "42");

    // Seconds (converts to long via operator int32_t)
    p.buf.clear();
    note::detail::print_json_value(p, note::Seconds{300});
    REQUIRE(p.buf == "300");

    // string_view
    p.buf.clear();
    note::detail::print_json_value(p, std::string_view("hello"));
    REQUIRE(p.buf == "\"hello\"");
}

// ---------------------------------------------------------------------------
// Response printTo
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: CardVersion response printTo") {
    Print p;
    note::api::CardVersion::Response rsp;
    rsp.version = "notecard-7.2.1";
    rsp.device = "dev:12345";
    rsp.printTo(p);
    REQUIRE(p.buf.find("notecard-7.2.1") != std::string::npos);
    REQUIRE(p.buf.find("dev:12345") != std::string::npos);
}
