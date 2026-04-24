// Target filtering — compile-time checks for Notecard compatibility.
//
// The Notecard comes in different hardware variants (WiFi, Cell, CellWifi,
// LoRa, Skylo) and endpoints may require a minimum firmware version. Target
// filtering catches unavailable at compile time instead of runtime.
//
// You can constrain the Api to a particular hardware variant, firmware version, or both.
// Using an unsupported API for that hardware/firmware combo produces a compiler warning (or a compile-time error in strict mode).
//
// Requires C++20. On C++17, all endpoints are available (no filtering).
//
// Build: clang++ -std=c++20 -fsyntax-only -I include examples/stdcpp/target-filtering.cpp

#include <note/api.hpp>
#include "mock_backend.hpp"

using namespace note;

int main() {
    MockBackend backend;
    MockTransport transport;
    Notecard nc(backend, transport);

    // ─── 1. Unconstrained API (default) ─────────────────────────────────
    // Without a target, all endpoints are available. This is fine when your
    // code runs on multiple Notecard variants, or when you don't care about
    // compile-time checks.

    Api api(nc);
    api.execute(api.card.sleep());    // OK: unconstrained API skips checks
    api.execute(api.hub.set());       // OK: universal endpoint

#if __cplusplus >= 202002L
    // ─── 2. Radios filtering ────────────────────────────────────────────
    // Constrain to a specific Notecard product family. Endpoints that don't
    // support this family produce deprecation warnings (or compile errors
    // in strict mode).

    Api wifi_api(nc, target<Radios::WiFi>());
    wifi_api.execute(wifi_api.card.sleep());   // OK: WiFi supports card.sleep
    wifi_api.execute(wifi_api.card.wifi());    // OK: WiFi-specific endpoint
    wifi_api.execute(wifi_api.hub.set());      // OK: universal
    // wifi_api.card.carrier();  // Would warn: card.carrier is not on WiFi

    // ─── 3. Firmware filtering ──────────────────────────────────────────
    // Constrain to a minimum firmware version. Endpoints that require a
    // newer firmware produce warnings.

    Api fw_api(nc, min_firmware<9, 1, 1>());
    fw_api.execute(fw_api.card.illumination()); // OK: added in 9.1.1
    fw_api.execute(fw_api.card.version());      // OK: universal
    // With an older firmware target, card.illumination would warn:
    // Api old_api(nc, min_firmware<5, 0, 0>());
    // old_api.card.illumination();  // Would warn: requires firmware >= 9.1.1

    // ─── 4. Combined hardware + firmware ────────────────────────────────
    // Both constraints checked simultaneously.

    Api both_api(nc, target<Radios::WiFi, 9, 1, 1>());
    both_api.execute(both_api.card.illumination()); // OK: fw >= 9.1.1
    both_api.execute(both_api.card.wifi());          // OK: WiFi hardware
    both_api.execute(both_api.hub.set());            // OK: universal

    // ─── 5. Compile-time introspection ──────────────────────────────────
    // Every request type carries static `hardware` and `min_firmware` fields.

    static_assert(api::CardSleep::radios.supports(Radios::WiFi));
    static_assert(!api::CardSleep::radios.supports(Radios::CellWifi));
    static_assert(api::HubSet::radios.supports(Radios::LoRa)); // universal

    static_assert(api::CardIllumination::min_firmware >= Firmware{9, 1, 1});
    static_assert(api::CardVersion::min_firmware == Firmware{}); // universal
#endif
}
