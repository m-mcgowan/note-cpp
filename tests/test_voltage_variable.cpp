// Tests for VoltageVariable: builder, string output, conversions.
#include <doctest.h>
#include <string>
#include <note/voltage_variable.hpp>
#include <string_view>

TEST_CASE("VoltageVariable single level") {
    note::VoltageVariable vv;
    vv.usb(5);
    CHECK(vv.str() == "usb:5");
}

TEST_CASE("VoltageVariable all levels") {
    note::VoltageVariable vv;
    vv.usb(5).high(15).normal(60).low(240).dead(0);
    CHECK(vv.str() == "usb:5;high:15;normal:60;low:240;dead:0");
}

TEST_CASE("VoltageVariable partial levels") {
    note::VoltageVariable vv;
    vv.high(10).low(120);
    CHECK(vv.str() == "high:10;low:120");
}

TEST_CASE("VoltageVariable empty") {
    note::VoltageVariable vv;
    CHECK(vv.str() == "");
    CHECK(vv.empty());
    CHECK_FALSE(static_cast<bool>(vv));
}

TEST_CASE("VoltageVariable operator bool") {
    note::VoltageVariable vv;
    vv.normal(30);
    CHECK(static_cast<bool>(vv));
    CHECK_FALSE(vv.empty());
}

TEST_CASE("VoltageVariable implicit string_view conversion") {
    note::VoltageVariable vv;
    vv.usb(5).dead(0);
    std::string_view sv = vv;
    CHECK(sv == "usb:5;dead:0");
}

TEST_CASE("VoltageVariable negative values") {
    note::VoltageVariable vv;
    vv.usb(-1);
    CHECK(vv.str() == "usb:-1");
}

TEST_CASE("VoltageVariable large values") {
    note::VoltageVariable vv;
    vv.usb(99999).high(99999).normal(99999).low(99999).dead(99999);
    // Should fit in 64-byte buffer: "usb:99999;high:99999;normal:99999;low:99999;dead:99999" = 55 chars
    CHECK(vv.str() == "usb:99999;high:99999;normal:99999;low:99999;dead:99999");
}

TEST_CASE("VoltageVariable builder returns self") {
    note::VoltageVariable vv;
    auto& ref = vv.usb(1);
    CHECK(&ref == &vv);
}
