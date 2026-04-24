// .assume() narrows to the asserted target — it is NOT a blanket
// "turn off checks" hatch. Filtering still applies against the asserted
// target, so calling a WiFi-only endpoint on an assumed LoRa SKU still
// fires the [[deprecated]] warning.
//
// Declared target = unconstrained (no filter of its own).
// Assumed target  = NOTE_LWEU (LoRa family) → card.wifi is NOT supported.
// Compile-warn runs under `-Werror=deprecated-declarations`, so the
// deprecation becomes an error; `WILL_FAIL TRUE` asserts compile-fail.
#include <note/api.hpp>
#include <note/sku_info.hpp>

void test(note::Api<note::Unconstrained>& api) {
    api.assume(note::sku::NOTE_LWEU).card.wifi();
}
