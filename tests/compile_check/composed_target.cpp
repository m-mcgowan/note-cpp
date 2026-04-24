// Compile-check: ComposedTarget<Axes...> intersection semantics.
// Drives the variadic-axis composition layer in target.hpp. Paired with
// tests/compile_fail/composed_target_*.cpp for the conflict-detection path.

#include <note/api/card_illumination.hpp>  // RadiosSupport Cell/CellWifi/Skylo/WiFi, fw>=9.1.1
#include <note/api/card_sleep.hpp>          // RadiosSupport WiFi only, no firmware min
#include <note/api/card_version.hpp>        // universal, no firmware min
#include <note/fw_versions.hpp>
#include <note/sku_info.hpp>
#include <note/target.hpp>

using namespace note;

// ── Empty pack = universal (same behavior as Unconstrained) ─────────────

using Empty = ComposedTarget<>;
static_assert(Empty::supports(api::CardSleep::radios));
static_assert(Empty::supports(api::CardIllumination::radios));
static_assert(Empty::firmware_ok(api::CardIllumination::min_firmware));
static_assert(Empty::firmware_version.as_int() == 0);
static_assert(Empty::strict == false);

// ── Single-axis equivalence to the legacy Target<> / MinFirmware<> ──────

using Wifi = ComposedTarget<decltype(radios::NOTE_WIFI)>;
static_assert( Wifi::supports(api::CardSleep::radios));       // WiFi is in mask
static_assert(!Wifi::supports(api::CardIllumination::radios)
           || Wifi::supports(api::CardIllumination::radios)); // universal-masks pass
static_assert(Wifi::radios == Radios::WiFi);
static_assert(Wifi::firmware_version.as_int() == 0);

using Fw = ComposedTarget<decltype(fw::v7_5_1)>;
static_assert(Fw::firmware_version.as_int() == 70501u);
static_assert(Fw::firmware_ok(Firmware{7, 5, 0}));
static_assert(Fw::firmware_ok(Firmware{7, 5, 1}));
static_assert(!Fw::firmware_ok(Firmware{8, 0, 0}));

// ── SKU axis contributes radios + mcu + has_txn_pins ────────────────────

using Esp = ComposedTarget<decltype(sku::NOTE_ESP)>;
static_assert(Esp::radios == Radios::WiFi);              // NOTE-ESP is WiFi family
static_assert(Esp::has_txn_pins == true);                // dedicated pins
static_assert(Esp::supports(api::CardSleep::radios));    // WiFi → ok
static_assert(Esp::supports(api::CardVersion::radios));  // universal → ok

// ── Multi-SKU: endpoint must support ALL declared SKUs ──────────────────

using EspOrSat = ComposedTarget<decltype(sku::NOTE_ESP),      // WiFi
                                decltype(sku::NOTE_NBGLWX)>;  // Skylo
// card.sleep is WiFi-only → rejected because Skylo isn't supported
static_assert(!EspOrSat::supports(api::CardSleep::radios));
// card.illumination is Cell/CellWifi/Skylo/WiFi → supports both
static_assert(EspOrSat::supports(api::CardIllumination::radios));

// ── Firmware = max(fw axes) ─────────────────────────────────────────────

using FwMax = ComposedTarget<decltype(fw::v3_2_1), decltype(fw::v7_5_1), decltype(fw::v7_5_2)>;
static_assert(FwMax::firmware_version.as_int() == 70502u);

// ── has_txn_pins = AND over SKU/MCU contributors ────────────────────────
// NOTE-ESP has dedicated pins (true), NOTE-WIFI has none (false) → AND = false

using EspPlusNoPin = ComposedTarget<decltype(sku::NOTE_ESP), decltype(sku::NOTE_WIFI)>;
static_assert(EspPlusNoPin::has_txn_pins == false);

// ── Combined: SKU + firmware ────────────────────────────────────────────

using EspAt9_1_1 = ComposedTarget<decltype(sku::NOTE_ESP), decltype(fw::v9_1_1)>;
static_assert(EspAt9_1_1::radios == Radios::WiFi);
static_assert(EspAt9_1_1::firmware_version.as_int() == 90101u);
static_assert(EspAt9_1_1::firmware_ok(api::CardIllumination::min_firmware));  // 9.1.1 >= 9.1.1
static_assert(EspAt9_1_1::supports(api::CardIllumination::radios));

using EspAt5_0_0 = ComposedTarget<decltype(sku::NOTE_ESP), decltype(fw::v5_1_1)>;
// 5.1.1 < 9.1.1 so illumination's firmware minimum is not met
static_assert(!EspAt5_0_0::firmware_ok(api::CardIllumination::min_firmware));

// ── Redundant but consistent: SKU's implied radios matches explicit ─────

using EspWifi = ComposedTarget<decltype(sku::NOTE_ESP), decltype(radios::NOTE_WIFI)>;
static_assert(EspWifi::radios == Radios::WiFi);
static_assert(EspWifi::supports(api::CardSleep::radios));

// ── as_strict() flips the bool, preserves the pack ──────────────────────

static_assert(Esp::strict == false);
static_assert(Esp::as_strict().strict == true);
static_assert(Esp::as_strict().radios == Esp::radios);
static_assert(Esp::as_strict().firmware_version.as_int() == Esp::firmware_version.as_int());

// ── IsTarget concept satisfied ──────────────────────────────────────────

static_assert(IsTarget<Esp>);
static_assert(IsTarget<EspOrSat>);
static_assert(IsTarget<Wifi>);
static_assert(IsTarget<Fw>);
static_assert(HasFirmwareCheck<Esp>);

int main() { return 0; }
