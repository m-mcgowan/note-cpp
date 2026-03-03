# Hub Configuration

Connection setup with type-safe units, named constants, and compile-time
validation.

> All example code on this page comes from [`main.cpp`](main.cpp), which is
> compiled as part of CI to verify correctness.

## 1. Basic hub.set

Two styles — fluent chaining or direct field assignment. Both produce the same
JSON.

**Fluent style:**

```cpp
// main.cpp#L85-L90

api.hubSet()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60)
    .inbound(120)
    .execute();
```

**Direct assignment:**

```cpp
// main.cpp#L94-L97

auto req = api.hubSet();
req.product = "com.example.app";
req.mode = "continuous";
req.execute();
```

## 2. Type-safe units

Duration fields use typed wrappers (`note::Minutes`, `note::Seconds`) so you
can't accidentally pass seconds where minutes are expected.

```cpp
// main.cpp#L106-L111

api.hubSet()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60_mins)       // Minutes literal
    .inbound(120_minutes)    // Long-form also works
    .seconds(300_s)          // Seconds literal
```

Raw integers still work — they implicitly convert to the correct unit type:

```cpp
// main.cpp#L114-L117

// Raw integers still work — they implicitly convert to the correct unit:
api.hubSet()
    .outbound(60)            // int → Minutes (outbound is in minutes)
    .seconds(300)            // int → Seconds (seconds is in seconds)
```

Mixing units is a compile error:

```cpp
// This would fail to compile:
//   api.hubSet().outbound(60_s);  // error: Seconds ≠ Minutes
```

## 3. Named constants

Special values like "reset to default" and "manual sync only" are named
constants on the field type. No need to remember magic numbers.

```cpp
// main.cpp#L129-L140


using outbound_t = note::api::HubSet::outbound_t;
using inbound_t  = note::api::HubSet::inbound_t;

// Reset outbound to default (sends -1 on the wire)
api.hubSet().outbound(outbound_t::reset).execute();

// Manual sync only — no automatic outbound (sends 0)
api.hubSet().outbound(outbound_t::manual).execute();

// Same constants exist for inbound
api.hubSet().inbound(inbound_t::reset).execute();
```

## 4. Consteval validation

Catch mode typos at compile time with `validatedMode()`. Invalid strings
trigger a compile error.

```cpp
// main.cpp#L149-L152

api.hubSet()
    .product("com.example.app")
    .mode(note::api::HubSet::validatedMode("periodic"))
    .execute();
```

## 5. Voltage-variable sync

Adapt sync frequency based on the Notecard's supply voltage. The Notecard
picks the interval matching its current voltage level.

**Raw string:**

```cpp
// main.cpp#L165-L168

api.hubSet()
    .mode("periodic")
    .voutbound("usb:5;high:15;normal:60;low:240;dead:0")
    .execute();
```

**Builder** — type-safe, built directly on the field:

```cpp
// main.cpp#L173-L177

auto req = api.hubSet();
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

```cpp
// main.cpp#L187-L204

api.hubSet()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60_mins)
    .uperiodic(true)
    .execute();

// Stay continuous on USB, fall back to minimum on battery
api.hubSet()
    .mode("minimum")
    .umin(true)
    .execute();

// Stay continuous on USB, fall back to off on battery
api.hubSet()
    .mode("off")
    .uoff(true)
    .execute();
```
