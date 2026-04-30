// Tests for note::arduino::I2cHal constructor variants.
//
// Verifies:
//  - Default ctor calls Wire.begin() with no args (managed bus, default pins).
//  - Pin-aware ctor calls Wire.begin(sda, scl) and replays it on reset.
//  - external_bus tag suppresses Wire.begin()/end() entirely (app owns bus).
//
// Compiled only under NOTE_ARDUINO_STUBS using the lightweight TwoWire
// stub in arduino_stubs.hpp.

#include <doctest.h>

#if defined(NOTE_ARDUINO_STUBS)

#include <note/arduino/i2c.hpp>

namespace {

TEST_CASE("I2cHal default ctor calls Wire.begin() with no pin args") {
    TwoWire wire;
    note::arduino::I2cHal hal(wire);

    REQUIRE(wire.begin_calls.size() == 1);
    CHECK(wire.begin_calls[0].sda == -1);
    CHECK(wire.begin_calls[0].scl == -1);
    CHECK(wire.end_count == 0);
}

TEST_CASE("I2cHal default reset() cycles end()/begin() with no pin args") {
    TwoWire wire;
    note::arduino::I2cHal hal(wire);
    wire.begin_calls.clear();

    REQUIRE(hal.reset());

    CHECK(wire.end_count == 1);
    REQUIRE(wire.begin_calls.size() == 1);
    CHECK(wire.begin_calls[0].sda == -1);
    CHECK(wire.begin_calls[0].scl == -1);
}

TEST_CASE("I2cHal pin-aware ctor calls Wire.begin(sda, scl)") {
    TwoWire wire;
    note::arduino::I2cHal hal(wire, /*sda=*/14, /*scl=*/21);

    REQUIRE(wire.begin_calls.size() == 1);
    CHECK(wire.begin_calls[0].sda == 14);
    CHECK(wire.begin_calls[0].scl == 21);
}

TEST_CASE("I2cHal pin-aware reset() replays Wire.begin(sda, scl)") {
    TwoWire wire;
    note::arduino::I2cHal hal(wire, 39, 38);
    wire.begin_calls.clear();

    REQUIRE(hal.reset());

    CHECK(wire.end_count == 1);
    REQUIRE(wire.begin_calls.size() == 1);
    CHECK(wire.begin_calls[0].sda == 39);
    CHECK(wire.begin_calls[0].scl == 38);
}

TEST_CASE("I2cHal external_bus ctor never touches the bus") {
    TwoWire wire;
    note::arduino::I2cHal hal(wire, note::arduino::external_bus);

    CHECK(wire.begin_calls.empty());
    CHECK(wire.end_count == 0);

    REQUIRE(hal.reset());

    CHECK(wire.begin_calls.empty());
    CHECK(wire.end_count == 0);
}

TEST_CASE("I2cHal external_bus ctor accepts custom address and MTU") {
    TwoWire wire;
    note::arduino::I2cHal hal(wire, note::arduino::external_bus, 0x42, 200);

    CHECK(wire.begin_calls.empty());
    CHECK(hal.max_transfer() == 200);
}

}  // namespace

#endif  // NOTE_ARDUINO_STUBS
