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

    .outbound(2_hours)       // Hours → Minutes (= 120 on the wire)
    .inbound(7_days)         // Days → Minutes (= 10080 on the wire)
    .execute();

// Works across the API — anywhere a Seconds field is expected:
api.card.sleep()                 // WiFi Notecard only
```

**Direct assignment:**

```cpp
// main.cpp#L94-L97

api.card.attn().arm()
    .triggers(note::attn::connected)  // flag constant via operator() — "arm," prepended automatically
    .seconds(5_mins)                  // Minutes → Seconds (= 300 on the wire)
    .execute();
```

## 2. Type-safe units

Duration fields use typed wrappers (`note::Minutes`, `note::Seconds`) so you
can't accidentally pass seconds where minutes are expected.

```cpp
// main.cpp#L106-L111







```

Raw integers still work — they implicitly convert to the correct unit type:

```cpp
// main.cpp#L114-L117


    // Same constants exist for inbound
    api.hub.set().inbound(inbound_t::reset).execute();
}
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


// This would fail at compile time:
//   .mode(note::api::HubSet::validatedMode("perioidc"))
//   error: "hub.set: invalid value for 'mode'"


// ═════════════════════════════════════════════════════════════════════════
// 5. Voltage-variable strings — adaptive sync based on power
// ═════════════════════════════════════════════════════════════════════════

// Raw string — works but easy to get the format wrong:
std::puts("\n--- Voltage-variable sync (raw string) ---");
```

## 4. Consteval validation

Catch mode typos at compile time with `validatedMode()`. Invalid strings
trigger a compile error.

```cpp
// main.cpp#L149-L152

auto req = api.hub.set();
req.mode = "periodic";
req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
```

## 5. Voltage-variable sync

Adapt sync frequency based on the Notecard's supply voltage. The Notecard
picks the interval matching its current voltage level.

**Raw string:**

```cpp
// main.cpp#L165-L168

.mode("periodic")
.outbound(60_mins)
.uperiodic(true)
.execute();
```

**Builder** — type-safe, built directly on the field:

```cpp
// main.cpp#L173-L177

    .umin(true)
    .execute();

// Stay continuous on USB, fall back to off on battery
api.hub.set()
```

Only levels you set are emitted — partial configurations are valid (e.g. just
`.voutbound.usb(5).normal(60)` produces `"usb:5;normal:60"`).

## 6. USB-variable booleans

Automatically switch between `continuous` mode on USB power and a fallback
mode on battery.

```cpp
// main.cpp#L187-L204


```
