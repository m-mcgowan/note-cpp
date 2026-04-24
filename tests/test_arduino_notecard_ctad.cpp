// Instantiates `note::arduino::Notecard` with a variadic axis pack so
// the Arduino-stubs build of note-cpp-tests-arduino exercises the CTAD
// code path in arduino.hpp. Without this, a bug in the arduino.hpp
// variadic constructor's `requires` clause (e.g. a wrong-namespace
// `detail::HasAxisCategory`) would only surface on the GitHub
// arduino-cli job — which has no local equivalent — while local
// compile-check and host-side tests would pass.
//
// Guarded on ARDUINO / NOTE_ARDUINO_STUBS so the host build skips it
// entirely; the file still must be in ALL_TEST_SOURCES to be picked up
// by the Arduino-stubs target.

#include "catch.hpp"

#if defined(ARDUINO) || defined(NOTE_ARDUINO_STUBS)

#include <note/arduino.hpp>
#include <note/fw_versions.hpp>
#include <note/sku_info.hpp>
#include <type_traits>

namespace n = note;

TEST_CASE("arduino::Notecard CTAD deduces ComposedTarget") {
    n::arduino::Notecard nc_default;
    static_assert(std::is_same_v<decltype(nc_default),
                                 n::arduino::Notecard<n::Unconstrained>>);

    n::arduino::Notecard nc_sku(n::sku::NOTE_ESP);
    static_assert(std::is_same_v<decltype(nc_sku),
        n::arduino::Notecard<n::ComposedTarget<n::SkuType<n::NotecardSku::NOTE_ESP>>>>);

    n::arduino::Notecard nc_sku_fw(n::sku::NOTE_ESP, n::fw::v7_5_1);
    static_assert(std::is_same_v<decltype(nc_sku_fw),
        n::arduino::Notecard<n::ComposedTarget<
            n::SkuType<n::NotecardSku::NOTE_ESP>, n::FwConstraint<7, 5, 1>>>>);

    REQUIRE(true);
}

#endif
