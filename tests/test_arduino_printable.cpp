// Tests for Arduino Printable support.
//
// When compiled with -DARDUINO (the arduino CMake target), verifies that
// generated printTo() methods compile and produce correct output.
// When compiled without ARDUINO, this file is empty.

#include "catch.hpp"

#ifdef ARDUINO

#include <note/api.hpp>
#include <note/error.hpp>

// ---------------------------------------------------------------------------
// ErrorInfo Printable
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: ErrorInfo printTo") {
    Print p;
    note::ErrorInfo e{note::Error::Notecard, "hello"};
    e.printTo(p);
    REQUIRE(p.buf == "notecard: hello");
}

// ---------------------------------------------------------------------------
// Request printTo
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: HubSet request printTo") {
    Print p;
    note::api::HubSet req;
    req.printTo(p);
    REQUIRE(p.buf.find("hub.set") != std::string::npos);
}

TEST_CASE("Arduino: CardAttn Arm with Seconds printTo") {
    Print p;
    note::api::CardAttn::Arm req;
    req.seconds = 300;
    req.printTo(p);
    REQUIRE(p.buf.find("300") != std::string::npos);
}

TEST_CASE("Arduino: HubSet with Minutes printTo") {
    Print p;
    note::api::HubSet req;
    req.outbound = 60;
    req.printTo(p);
    REQUIRE(p.buf.find("60") != std::string::npos);
}

// ---------------------------------------------------------------------------
// print_json_value dispatch
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: print_json_value dispatch") {
    Print p;
    using note::detail::print_json_value;

    p.buf.clear();
    print_json_value(p, true);
    REQUIRE(p.buf == "true");

    p.buf.clear();
    print_json_value(p, std::string_view("hello"));
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

#endif // ARDUINO
