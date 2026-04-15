# Hub Configuration and Sync Settings

Connection setup with type-safe units, named constants, and compile-time
validation.

> All example code on this page comes from [`main.cpp`](main.cpp), which is
> compiled as part of CI to verify correctness.

## 1. Basic hub.set

Two styles — fluent chaining or direct field assignment. Both produce the same
JSON.

**Fluent style:**

<!-- snippet:fluent examples/hub-configuration/main.cpp:44-49 -->
```cpp
api.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60)
    .inbound(120)
    .execute();
```

**Direct assignment:**

<!-- snippet:direct examples/hub-configuration/main.cpp:55-58 -->
```cpp
auto req = api.hub.set();
req.product = "com.example.app";
req.mode = "continuous";
req.execute();
```

## 2. Type-safe units

Duration fields use typed wrappers (`note::Minutes`, `note::Seconds`) so you
can't accidentally pass seconds where minutes are expected.

<!-- snippet:units examples/hub-configuration/main.cpp:69-75 -->
```cpp
api.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60_mins)       // Minutes literal
    .inbound(120_minutes)    // Long-form also works
    .seconds(300_s)          // Seconds literal
    .execute();
```

Raw integers still work — they implicitly convert to the correct unit type:

<!-- snippet:units-raw examples/hub-configuration/main.cpp:80-83 -->
```cpp
api.hub.set()
    .outbound(60)            // int → Minutes (outbound is in minutes)
    .seconds(300)            // int → Seconds (seconds is in seconds)
    .execute();
```

Hours and Days convert to smaller units automatically:

<!-- snippet:hours-days examples/hub-configuration/main.cpp:92-107 -->
```cpp
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
```

Mixing units is a compile error:

```cpp
// This would fail to compile:
//   api.hub.set().outbound(60_s);  // error: Seconds ≠ Minutes
```

## 3. Named constants

Special values like "reset to default" and "manual sync only" are named
constants on the field type. No need to remember magic numbers.

Named constants are also available: `HubSet::mode_t::periodic`, `HubSet::mode_t::continuous`, etc.

<!-- snippet:named-constants examples/hub-configuration/main.cpp:118-125 -->
```cpp
// Reset outbound to default (sends -1 on the wire)
api.hub.set().outbound(-1).execute();

// Manual sync only — no automatic outbound (sends 0)
api.hub.set().outbound(0).execute();

// Same constants exist for inbound
api.hub.set().inbound(-1).execute();
```

## 4. Compile-time validation

On C++20, mode string literals are validated at compile time automatically — typos are caught without any special syntax. On C++17, use `validatedMode()` for the same compile-time check:
<!-- snippet:consteval examples/hub-configuration/main.cpp:136-143 -->
```cpp
api.hub.set()
    .product("com.example.app")
    .mode(note::api::HubSet::validatedMode("periodic"))
    .execute();

// This would fail at compile time:
//   .mode(note::api::HubSet::validatedMode("perioidc"))
//   error: "hub.set: invalid value for 'mode'"
```

## 5. Voltage-variable sync

Adapt sync frequency based on the Notecard's supply voltage. The Notecard
picks the interval matching its current voltage level.

**Raw string:**

<!-- snippet:vvar-raw examples/hub-configuration/main.cpp:154-157 -->
```cpp
api.hub.set()
    .mode("periodic")
    .voutbound("usb:5;high:15;normal:60;low:240;dead:0")
    .execute();
```

**Builder** — type-safe, built directly on the field:

<!-- snippet:vvar-builder examples/hub-configuration/main.cpp:164-168 -->
```cpp
auto req = api.hub.set();
req.mode = "periodic";
req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
req.execute();
```

Only levels you set are emitted — partial configurations are valid (e.g. just
`.voutbound.usb(5).normal(60)` produces `"usb:5;normal:60"`).

## 6. USB-variable booleans

Automatically switch between `continuous` mode on USB power and a fallback
mode on battery.


Stay continuous on USB power, fall back to periodic on battery:

<!-- snippet:usb-variable examples/hub-configuration/main.cpp:179-197 -->
```cpp
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
```
