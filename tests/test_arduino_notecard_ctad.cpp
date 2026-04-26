// Compile-time check that note::arduino::Notecard's CTAD picks the right
// template argument for the default ctor and the variadic axis-pack ctor.
// Without this, a bug in the arduino.hpp variadic constructor's `requires`
// clause (e.g. a wrong-namespace `detail::HasAxisCategory`) would only
// surface on the GitHub arduino-cli job — which has no local equivalent —
// while local compile-check and host-side tests would pass.
//
// The deductions are pure compile-time checks: `decltype` evaluates without
// invoking the constructor, so no instance lands on the stack. (Earlier
// versions of this file declared three on-stack Notecards inside a single
// TEST_CASE body and blew the ESP32-S3 loopTask stack.)

#include <doctest.h>

#if defined(ARDUINO) || defined(NOTE_ARDUINO_STUBS)

#include <note/arduino.hpp>
#include <note/fw_versions.hpp>
#include <note/sku_info.hpp>
#include <type_traits>

namespace n = note;

// CTAD deductions — namespace-scope static_asserts. Compile-only; no runtime
// cost, no object instantiation.
static_assert(std::is_same_v<
    decltype(n::arduino::Notecard{}),
    n::arduino::Notecard<n::Unconstrained>
>);

static_assert(std::is_same_v<
    decltype(n::arduino::Notecard{n::sku::NOTE_ESP}),
    n::arduino::Notecard<n::ComposedTarget<n::SkuType<n::NotecardSku::NOTE_ESP>>>
>);

static_assert(std::is_same_v<
    decltype(n::arduino::Notecard{n::sku::NOTE_ESP, n::fw::v7_5_1}),
    n::arduino::Notecard<n::ComposedTarget<
        n::SkuType<n::NotecardSku::NOTE_ESP>, n::FwConstraint<7, 5, 1>>>
>);

// Sentinel test_case so doctest reports a passing case for this file.
TEST_CASE("arduino::Notecard CTAD deductions (compile-time)") {
    REQUIRE(true);
}

#endif
