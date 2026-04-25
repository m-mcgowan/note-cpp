// Tests for Arduino Printable support.
//
// When compiled with -DARDUINO (the arduino CMake target), verifies that
// generated printTo() methods compile and produce correct output.
// When compiled without ARDUINO, this file is empty.

#include <doctest.h>
#include <string>

#ifdef ARDUINO

#include <note/api.hpp>
#include <note/error.hpp>
#include <cstring>

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

// ---------------------------------------------------------------------------
// printable_string_view — ResponseArray element printing
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: printable_string_view has printTo") {
    Print p;
    note::printable_string_view sv("hello world");
    sv.printTo(p);
    REQUIRE(p.buf == "hello world");
}

TEST_CASE("Arduino: printable_string_view converts from string_view") {
    note::string_view plain = "test";
    note::printable_string_view psv = plain;
    REQUIRE(psv == "test");
    REQUIRE(psv.size() == 4);
}

TEST_CASE("Arduino: printable_string_view works with printable() wrapper") {
    Print p;
    note::printable_string_view sv("wrapped");
    auto w = note::printable(sv);
    w.printTo(p);
    REQUIRE(p.buf == "wrapped");
}

// ---------------------------------------------------------------------------
// c_str() — null-terminated C string access
// ---------------------------------------------------------------------------

TEST_CASE("Arduino: printable_string_view c_str() returns null-terminated") {
    note::printable_string_view sv("hello");
    const char* cs = sv.c_str();
    REQUIRE(cs == sv.data());
    REQUIRE(std::strcmp(cs, "hello") == 0);
}

TEST_CASE("Arduino: ResponseField<string_view> c_str()") {
    note::ResponseField<note::string_view> field;
    field = note::string_view("world");
    const char* cs = field.c_str();
    REQUIRE(std::strcmp(cs, "world") == 0);
}


#endif // ARDUINO
