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

api.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .outbound(60)
    .inbound(120)
    .execute();
```

**Direct assignment:**

```cpp
// main.cpp#L94-L97

auto req = api.hub.set();
req.product = "com.example.app";
req.mode = "continuous";
req.execute();
```

## 2. Type-safe units

Duration fields use typed wrappers (`note::Minutes`, `note::Seconds`) so you
can't accidentally pass seconds where minutes are expected.

```cpp
// main.cpp#L106-L111

api.hub.set()
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
api.hub.set()
    .outbound(60)            // int → Minutes (outbound is in minutes)
    .seconds(300)            // int → Seconds (seconds is in seconds)
```

Mixing units is a compile error:

```cpp
// This would fail to compile:
//   api.hub.set().outbound(60_s);  // error: Seconds ≠ Minutes
```

## 3. Named constants

Special values like "reset to default" and "manual sync only" are named
constants on the field type. No need to remember magic numbers.

```cpp
// main.cpp#L129-L140

    .inbound(7_days)         // Days → Minutes (= 10080 on the wire)
    .execute();

// Works across the API — anywhere a Seconds field is expected:
api.card.sleep()
    .seconds(12_hours)       // Hours → Seconds (= 43200 on the wire)
    .execute();

api.card.attn().arm()
    .mode("arm")
    .seconds(5_mins)         // Minutes → Seconds (= 300 on the wire)
    .execute();
```

## 4. Consteval validation

Catch mode typos at compile time with `validatedMode()`. Invalid strings
trigger a compile error.

```cpp
// main.cpp#L149-L152

using outbound_t = note::api::HubSet::outbound_t;
using inbound_t  = note::api::HubSet::inbound_t;

// Reset outbound to default (sends -1 on the wire)
```

## 5. Voltage-variable sync

Adapt sync frequency based on the Notecard's supply voltage. The Notecard
picks the interval matching its current voltage level.

**Raw string:**

```cpp
// main.cpp#L165-L168

// ═════════════════════════════════════════════════════════════════════════

std::puts("\n--- Consteval validation ---");
api.hub.set()
```

**Builder** — type-safe, built directly on the field:

```cpp
// main.cpp#L173-L177






```

Only levels you set are emitted — partial configurations are valid (e.g. just
`.voutbound.usb(5).normal(60)` produces `"usb:5;normal:60"`).

## 6. USB-variable booleans

Automatically switch between `continuous` mode on USB power and a fallback
mode on battery.

```cpp
// main.cpp#L187-L204

    .execute();

// Builder — type-safe, built directly on the field:
std::puts("\n--- Voltage-variable sync (builder) ---");
{
    auto req = api.hub.set();
    req.mode = "periodic";
    req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
    req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
    req.execute();
}


// ═════════════════════════════════════════════════════════════════════════
// 6. USB-variable booleans — automatic mode switching on USB power
// ═════════════════════════════════════════════════════════════════════════

std::puts("\n--- USB-variable sync ---");
```
