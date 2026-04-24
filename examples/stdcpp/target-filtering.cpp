// Target filtering — compile-time checks for Notecard compatibility.
//
// The Notecard ships in different hardware variants (WiFi, Cell, CellWifi,
// LoRa, Skylo) and individual endpoints may require a minimum firmware
// version. Target filtering catches unavailable endpoints at compile time
// instead of at runtime.
//
// You declare the target at construction. Unsupported endpoints then
// produce a deprecation warning (or a hard error in strict mode).
// The filter is a guiderail, not a fence — for a single call you can
// widen the target via `nc.assume(...)`.
//
// Requires C++20. On C++17 the filters compile away and all endpoints
// are available.
//
// Build: c++ -std=c++20 -I include examples/stdcpp/target-filtering.cpp

#include <note/api.hpp>
#include <note/fw_versions.hpp>
#include <note/notecard_api.hpp>
#include <note/sku_info.hpp>
#include "mock_backend.hpp"

#include <type_traits>

using namespace note;

int main() {
    MockBackend backend;
    MockTransport transport;
    Notecard nc(backend, transport);

    // ─── 1. Unconstrained API (default) ─────────────────────────────────
    // With no target, all endpoints are available and nothing is filtered.
    // Use this when the same code runs on multiple Notecard variants.

    Api api(nc);
    api.execute(api.card.sleep());    // OK
    api.execute(api.hub.set());       // OK

#if __cplusplus >= 202002L
    // ─── 2. Radios filtering (legacy Target<> form) ─────────────────────
    // Constrain to a specific product family. Endpoints that don't apply
    // to that family warn (non-strict) or error (strict).

    Api wifi_api(nc, target<Radios::WiFi>());
    wifi_api.execute(wifi_api.card.sleep());   // OK: WiFi supports card.sleep
    wifi_api.execute(wifi_api.card.wifi());    // OK: WiFi-specific endpoint
    wifi_api.execute(wifi_api.hub.set());      // OK: universal
    // wifi_api.card.carrier();  // Would warn: card.carrier is not on WiFi

    // ─── 3. Firmware filtering ──────────────────────────────────────────
    // Constrain to a minimum firmware version.

    Api fw_api(nc, min_firmware<9, 1, 1>());
    fw_api.execute(fw_api.card.illumination()); // OK: added in 9.1.1
    fw_api.execute(fw_api.card.version());      // OK: universal
    // Api old_api(nc, min_firmware<5, 0, 0>());
    // old_api.card.illumination();  // Would warn: requires firmware >= 9.1.1

    // ─── 4. Combined hardware + firmware (legacy form) ──────────────────

    Api both_api(nc, target<Radios::WiFi, 9, 1, 1>());
    both_api.execute(both_api.card.illumination()); // OK
    both_api.execute(both_api.card.wifi());         // OK
    both_api.execute(both_api.hub.set());           // OK

    // ─── 5. Axis-based construction (preferred DX) ──────────────────────
    // The Notecard wrapper deduces the target from axis values at the
    // call site — no angle brackets, no Target<> verbosity. Axes:
    //
    //   note::sku::NOTE_*      — specific product (25 values)
    //   note::radios::NOTE_*   — family: CELL / WIFI / CELL_WIFI / LORA / SKYLO
    //   note::mcu::NOTE_*      — internal MCU (escape hatch for custom boards)
    //   note::fw::v*           — minimum firmware (codegenned thresholds)
    //
    // Axes compose: same axis → intersection; cross-axis conflict is a
    // compile error (e.g. sku::NOTE_ESP + radios::NOTE_CELL).

    NotecardApi nc_any;                                            // Unconstrained
    NotecardApi nc_esp  {sku::NOTE_ESP};                            // specific SKU
    NotecardApi nc_fw   {sku::NOTE_ESP, fw::v9_1_1};                // SKU + firmware
    NotecardApi nc_wifi {radios::NOTE_WIFI};                        // family
    NotecardApi nc_stm  {mcu::NOTE_STM32L4};                        // custom board
    NotecardApi nc_multi{sku::NOTE_NBGLN, sku::NOTE_NBGLW};         // intersection

    static_assert(std::is_same_v<decltype(nc_any),  NotecardApi<Unconstrained>>);
    static_assert(std::is_same_v<decltype(nc_esp),
                  NotecardApi<ComposedTarget<SkuType<NotecardSku::NOTE_ESP>>>>);
    static_assert(std::is_same_v<decltype(nc_wifi),
                  NotecardApi<ComposedTarget<RadiosType<Radios::WiFi>>>>);

    // Silence "unused" warnings for the illustrative wrappers above.
    (void)nc_any; (void)nc_fw; (void)nc_stm; (void)nc_multi;

    // ─── 6. assume() — scoped target-widening escape hatch ──────────────
    // Target filtering is a guiderail. For a runtime-conditional per-SKU
    // branch, assume() says "for this one call, treat me as this target."
    //
    //   note::NotecardApi nc(note::radios::NOTE_CELL);
    //   if (runtime_is_note_esp()) {
    //       nc.assume(note::sku::NOTE_ESP).card.sleep().execute();
    //   }
    //
    // assume() *replaces* the declared target for the returned proxy;
    // filtering still applies against the asserted target. So
    // `assume(sku::NOTE_LWEU).card.wifi()` still warns — LoRa SKUs
    // don't carry WiFi.

    // Demo: the outer api is Unconstrained, but we assume an ESP SKU.
    // The assumed target is typed with SkuType<NOTE_ESP>.
    auto esp_scope = api.assume(sku::NOTE_ESP);
    static_assert(std::is_same_v<
        decltype(esp_scope),
        Api<ComposedTarget<SkuType<NotecardSku::NOTE_ESP>>, Notecard>>);
    esp_scope.execute(esp_scope.card.sleep());

    // ─── 7. Compile-time introspection ──────────────────────────────────
    // Every endpoint carries static `radios` and `min_firmware` members.

    static_assert(api::CardSleep::radios.supports(Radios::WiFi));
    static_assert(!api::CardSleep::radios.supports(Radios::CellWifi));
    static_assert(api::HubSet::radios.supports(Radios::LoRa)); // universal

    static_assert(api::CardIllumination::min_firmware >= Firmware{9, 1, 1});
    static_assert(api::CardVersion::min_firmware == Firmware{}); // universal
#endif
}
