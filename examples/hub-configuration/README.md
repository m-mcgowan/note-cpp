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

    .mode("periodic")
    .outbound(2_hours)       // Hours → Minutes (= 120 on the wire)
    .inbound(7_days)         // Days → Minutes (= 10080 on the wire)
    .execute();

// Works across the API — anywhere a Seconds field is expected:
```

**Direct assignment:**

```cpp
// main.cpp#L94-L97


api.card.attn().arm()
    .triggers(note::attn::connected)  // flag constant via operator() — "arm," prepended automatically
    .seconds(5_mins)                  // Minutes → Seconds (= 300 on the wire)
```

## 2. Type-safe units

Duration fields use typed wrappers (`note::Minutes`, `note::Seconds`) so you
can't accidentally pass seconds where minutes are expected.

```cpp
// main.cpp#L106-L111


// Reset outbound to default (sends -1 on the wire)
api.hub.set().outbound(-1).execute();

// Manual sync only — no automatic outbound (sends 0)
api.hub.set().outbound(0).execute();
```

Raw integers still work — they implicitly convert to the correct unit type:

```cpp
// main.cpp#L114-L117





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

//   .mode(note::api::HubSet::validatedMode("perioidc"))
//   error: "hub.set: invalid value for 'mode'"


// ═════════════════════════════════════════════════════════════════════════
// 5. Voltage-variable strings — adaptive sync based on power
// ═════════════════════════════════════════════════════════════════════════

// Raw string — works but easy to get the format wrong:
std::puts("\n--- Voltage-variable sync (raw string) ---");
api.hub.set()
    .mode("periodic")
```

## 4. Consteval validation

Catch mode typos at compile time with `validatedMode()`. Invalid strings
trigger a compile error.

```cpp
// main.cpp#L149-L152

    req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
    req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
    req.execute();
}
```

## 5. Voltage-variable sync

Adapt sync frequency based on the Notecard's supply voltage. The Notecard
picks the interval matching its current voltage level.

**Raw string:**

```cpp
// main.cpp#L165-L168

    .uperiodic(true)
    .execute();

// Stay continuous on USB, fall back to minimum on battery
```

**Builder** — type-safe, built directly on the field:

```cpp
// main.cpp#L173-L177


// Stay continuous on USB, fall back to off on battery
api.hub.set()
    .mode("off")
    .uoff(true)
```

Only levels you set are emitted — partial configurations are valid (e.g. just
`.voutbound.usb(5).normal(60)` produces `"usb:5;normal:60"`).

## 6. USB-variable booleans

Automatically switch between `continuous` mode on USB power and a fallback
mode on battery.

```cpp
// main.cpp#L187-L204


```
