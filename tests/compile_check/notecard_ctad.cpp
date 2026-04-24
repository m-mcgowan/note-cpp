// Compile-check: the variadic-axis CTAD on the Notecard wrappers maps
// axis values at the call site to `ComposedTarget<Axes...>`, without
// interfering with the zero-arg / transport / backend constructors.

#include <note/fw_versions.hpp>
#include <note/notecard_api.hpp>
#include <note/sku_info.hpp>
#include <note/target.hpp>
#include <type_traits>

namespace n = note;

// ── Zero-arg: stays Unconstrained ───────────────────────────────────────

void test_unconstrained() {
    n::NotecardApi nc;
    static_assert(std::is_same_v<decltype(nc), n::NotecardApi<n::Unconstrained>>);
}

// ── Single SKU axis ─────────────────────────────────────────────────────

void test_single_sku() {
    n::NotecardApi nc(n::sku::NOTE_ESP);
    using Expected = n::NotecardApi<
        n::ComposedTarget<n::SkuType<n::NotecardSku::NOTE_ESP>>>;
    static_assert(std::is_same_v<decltype(nc), Expected>);
}

// ── SKU + firmware ──────────────────────────────────────────────────────

void test_sku_and_fw() {
    n::NotecardApi nc(n::sku::NOTE_ESP, n::fw::v7_5_1);
    using Expected = n::NotecardApi<
        n::ComposedTarget<n::SkuType<n::NotecardSku::NOTE_ESP>,
                          n::FwConstraint<7, 5, 1>>>;
    static_assert(std::is_same_v<decltype(nc), Expected>);
}

// ── Radios axis ─────────────────────────────────────────────────────────

void test_radios() {
    n::NotecardApi nc(n::radios::NOTE_WIFI);
    using Expected = n::NotecardApi<
        n::ComposedTarget<n::RadiosType<n::Radios::WiFi>>>;
    static_assert(std::is_same_v<decltype(nc), Expected>);
}

// ── MCU axis ────────────────────────────────────────────────────────────

void test_mcu() {
    n::NotecardApi nc(n::mcu::NOTE_STM32L4);
    using Expected = n::NotecardApi<
        n::ComposedTarget<n::McuType<n::Mcu::Stm32L4>>>;
    static_assert(std::is_same_v<decltype(nc), Expected>);
}

// ── Multi-SKU: two SKUs intersect into one ComposedTarget ───────────────

void test_multi_sku() {
    n::NotecardApi nc(n::sku::NOTE_ESP, n::sku::NOTE_NBGLWX);
    using Expected = n::NotecardApi<
        n::ComposedTarget<n::SkuType<n::NotecardSku::NOTE_ESP>,
                          n::SkuType<n::NotecardSku::NOTE_NBGLWX>>>;
    static_assert(std::is_same_v<decltype(nc), Expected>);
}

int main() { return 0; }
