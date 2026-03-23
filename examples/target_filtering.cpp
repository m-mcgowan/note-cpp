// Target filtering — compile-time checks for Notecard hardware compatibility.
//
// The Notecard comes in different variants: WiFi, Cell, LoRa, Skylo. Some
// API endpoints only work on specific hardware — for example, card.wifi is
// only available on WiFi Notecards. If you accidentally call card.wifi on
// a cellular device, the Notecard returns an error at runtime.
//
// Target filtering catches this at compile time instead. Constrain your Api
// to a specific product, and unsupported endpoints produce compiler warnings
// (or errors in strict mode). This is especially useful when you're building
// firmware that targets a specific Notecard variant.
//
// Requires C++20. On C++17, all endpoints are available (no filtering).
//
// Build: clang++ -std=c++20 -fsyntax-only -I include examples/target_filtering.cpp

#include <note/api.hpp>
#include "mock_backend.hpp"

int main() {
    MockBackend backend;
    MockTransport transport;
    note::Notecard nc(backend, transport);

    // ─── 1. Unconstrained API (default) ─────────────────────────────────
    // Without a target, all endpoints are available. This is fine when your
    // code runs on multiple Notecard variants, or when you don't care about
    // compile-time hardware checks.

    note::Api api(nc);
    api.execute(api.card.sleep());    // OK: no constraints
    api.execute(api.hub.set());       // OK: universal endpoint

#if __cplusplus >= 202002L
    // ─── 2. WiFi target ─────────────────────────────────────────────────
    // Constrain to WiFi Notecard. WiFi-specific endpoints compile normally.
    // Endpoints that don't support WiFi produce deprecation warnings (or
    // compile errors if NOTE_API_STRICT is defined).

    note::Api wifi_api(nc, note::target<note::Product::WiFi>());
    wifi_api.execute(wifi_api.card.sleep());   // OK: WiFi supports card.sleep
    wifi_api.execute(wifi_api.card.wifi());    // OK: WiFi-specific endpoint
    wifi_api.execute(wifi_api.hub.set());      // OK: universal

    // ─── 3. Custom target with RAT composition ──────────────────────────
    // Notecard SKUs are defined by their Radio Access Technologies (RATs).
    // You can compose custom targets for specialized hardware. Skylo, for
    // example, supports both satellite (NTN) and WiFi RATs.

    constexpr auto skylo_rats = note::Product::Cell + note::Rat::Ntn;
    note::Api skylo_api(nc, note::target<note::Product::Skylo>());
    skylo_api.execute(skylo_api.card.wifi());  // OK: Skylo has WiFi RAT
    skylo_api.execute(skylo_api.hub.set());    // OK: universal

    // ─── 4. Compile-time SKU introspection ──────────────────────────────
    // Every request type carries a static `skus` field listing which RATs
    // it supports. You can query this at compile time.

    static_assert(note::api::CardSleep::skus.supports(note::Rat::WiFi));
    static_assert(!note::api::CardSleep::skus.supports(note::Rat::LoRa));
    static_assert(note::api::HubSet::skus.supports(note::Rat::LoRa)); // universal

    (void)skylo_rats;
#endif
}
