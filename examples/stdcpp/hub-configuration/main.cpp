// Hub configuration — setting up the Notecard's connection to Notehub.
//
// The first thing most applications do is configure how the Notecard syncs
// with Notehub: which product it belongs to, how often it connects, and
// whether to use continuous or periodic mode. This example shows how to
// do that with type-safe units that prevent accidental mixing of minutes
// and seconds, named constants that replace magic numbers, and compile-time
// validation that catches typos before they reach the device.
//
// All example code in the accompanying README comes from this file, which is
// compiled as part of CI to verify correctness.
//
// Build & run:
//   c++ -std=c++20 -I ../../include main.cpp && ./a.out

#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/units.hpp>
#include <note/voltage_variable.hpp>

#include "../mock_backend.hpp"
#include <cstdio>


int main() {
    MockBackend backend;
    note::CallbackTransport transport(
        [](note::string_view request, uint32_t) -> note::Result<note::string_view> {
            std::printf("  >> %.*s\n", (int)request.size(), request.data());
            return note::string_view("{}");
        });
    note::Notecard nc(backend, transport);

    note::Api api(nc);
    using namespace note::literals;


    // ═════════════════════════════════════════════════════════════════════════
    // 1. Basic hub.set — fluent and direct styles
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Fluent style ---");
    // readme:fluent
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60)
        .inbound(120)
        .execute();
    // readme:end

    std::puts("\n--- Direct assignment ---");
    {
        // readme:direct
        auto req = api.hub.set();
        req.product = "com.example.app";
        req.mode = "continuous";
        req.execute();
        // readme:end
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 2. Type-safe units — prevent accidental unit mixing
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Type-safe units ---");
    // readme:units
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)       // Minutes literal
        .inbound(120_minutes)    // Long-form also works
        .seconds(300_s)          // Seconds literal
        .execute();
    // readme:end

    // Raw integers still work — they implicitly convert to the correct unit:
    // readme:units-raw
    api.hub.set()
        .outbound(60)            // int → Minutes (outbound is in minutes)
        .seconds(300)            // int → Seconds (seconds is in seconds)
        .execute();
    // readme:end

    // But you can't mix them up — this would be a compile error:
    //   api.hubSet().outbound(60_s);  // error: Seconds ≠ Minutes

    // Hours and Days convert implicitly to smaller units:
    std::puts("\n--- Hours and Days ---");
    // readme:hours-days
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(2_hours)       // Hours → Minutes (= 120 on the wire)
        .inbound(7_days)         // Days → Minutes (= 10080 on the wire)
        .execute();

    // Works across the API — anywhere a Seconds field is expected:
    api.card.sleep()                 // WiFi Notecard only
        .seconds(12_hours)       // Hours → Seconds (= 43200 on the wire)
        .execute();

    api.card.attn().arm()
        .triggers(note::attn::connected)  // flag constant via operator() — "arm," prepended automatically
        .seconds(5_mins)                  // Minutes → Seconds (= 300 on the wire)
        .execute();
    // readme:end


    // ═════════════════════════════════════════════════════════════════════════
    // 3. Named constants — self-documenting special values
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Named constants ---");
    {
        // readme:named-constants
        // Reset outbound to default (sends -1 on the wire)
        api.hub.set().outbound(-1).execute();

        // Manual sync only — no automatic outbound (sends 0)
        api.hub.set().outbound(0).execute();

        // Same constants exist for inbound
        api.hub.set().inbound(-1).execute();
        // readme:end
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 4. Consteval validation — catch typos at compile time
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Consteval validation ---");
    // readme:consteval
    api.hub.set()
        .product("com.example.app")
        .mode(note::api::HubSet::validatedMode("periodic"))
        .execute();

    // This would fail at compile time:
    //   .mode(note::api::HubSet::validatedMode("perioidc"))
    //   error: "hub.set: invalid value for 'mode'"
    // readme:end


    // ═════════════════════════════════════════════════════════════════════════
    // 5. Voltage-variable strings — adaptive sync based on power
    // ═════════════════════════════════════════════════════════════════════════

    // Voltage-variable sync adjusts how often the Notecard syncs based on
    // the current power level (USB, high battery, normal, low, dead).
    // This saves battery — sync less often when power is low.

    // Raw string — works but the format is easy to get wrong:
    std::puts("\n--- Voltage-variable sync (raw string) ---");
    // readme:vvar-raw
    api.hub.set()
        .mode("periodic")
        .voutbound("usb:5;high:15;normal:60;low:240;dead:0")  // "level:minutes" pairs
        .execute();
    // readme:end

    // Builder — type-safe, each power level is a named method:
    std::puts("\n--- Voltage-variable sync (builder) ---");
    {
        // readme:vvar-builder
        auto req = api.hub.set();
        req.mode = "periodic";
        // Outbound sync interval (minutes) at each power level:
        req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
        // Inbound check interval (minutes) at each power level:
        req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
        req.execute();
        // readme:end
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 6. USB-variable booleans — automatic mode switching on USB power
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- USB-variable sync ---");
    // readme:usb-variable
    // Stay continuous on USB, fall back to periodic on battery
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)
        .uperiodic(true)
        .execute();

    // Stay continuous on USB, fall back to minimum on battery
    api.hub.set()
        .mode("minimum")
        .umin(true)
        .execute();

    // Stay continuous on USB, fall back to off on battery
    api.hub.set()
        .mode("off")
        .uoff(true)
        .execute();
    // readme:end


    std::puts("\nAll hub-configuration examples completed.");
    return 0;
}
