# Type-Safe Duration Units

The Notecard API uses different time units across endpoints — `hub.set` outbound/inbound are in minutes, `card.attn` seconds is in seconds, and so on. In `note-c`, these are all plain integers and mixing them up is a silent bug. `note-cpp` uses distinct types that prevent this at compile time.

## Unit types

| Type | On the wire | Literals | Accepts (implicit) |
|------|-------------|----------|-------------------|
| `Days` | days | `7_days`, `7_d` | — |
| `Hours` | hours | `2_hours`, `2_hr`, `2_h` | `Days` |
| `Minutes` | minutes | `60_mins`, `60_minutes` | `Hours`, `Days` |
| `Seconds` | seconds | `300_s`, `300_seconds` | `Minutes`, `Hours`, `Days` |
| `Milliseconds` | milliseconds | `500_ms` | `Seconds`, `Minutes`, `Hours`, `Days` |

Conversion is one-directional: larger units implicitly convert to smaller ones (no precision loss). Going the other way requires explicit construction.

## Usage

```cpp
using namespace note::literals;

// Each field knows its unit type — IDE autocomplete tells you
api.hub.set()
    .outbound(15_mins)       // Minutes literal
    .inbound(7_days)         // Days → Minutes (= 10080 on the wire)
    .execute();

api.card.attn()
    .seconds(5_mins)         // Minutes → Seconds (= 300 on the wire)
    .execute();

api.card.sleep()
    .seconds(12_hours)       // Hours → Seconds (= 43200 on the wire)
    .execute();
```

## Raw integers still work

Raw `int32_t` values implicitly convert to the correct unit type, so existing code doesn't break:

```cpp
api.hub.set().outbound(60);      // int → Minutes
api.hub.set().seconds(300);      // int → Seconds
```

## Compile-time safety

Wrong-direction conversions are compile errors:

```cpp
api.hub.set().outbound(300_s);   // error: Seconds cannot convert to Minutes
api.card.attn().seconds(5_mins); // OK: Minutes → Seconds (implicit)
```

## Named constants

Special values are named constants on the field type, replacing magic numbers:

```cpp
using outbound_t = note::api::HubSet::outbound_t;

api.hub.set().outbound(outbound_t::reset).execute();   // sends -1 (reset to default)
api.hub.set().outbound(outbound_t::manual).execute();   // sends 0 (manual sync only)
```

## Voltage-variable sync

The Notecard's `voutbound`/`vinbound` fields adapt sync frequency to supply voltage. A type-safe builder constructs the semicolon-delimited string:

```cpp
auto req = api.hub.set();
req.mode = "periodic";
req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
req.execute();
// voutbound on the wire: "usb:5;high:15;normal:60;low:240;dead:0"
```

Only levels you set are emitted — partial configurations are valid:

```cpp
req.voutbound.usb(5).normal(60);
// produces: "usb:5;normal:60"
```

## Implementation

All types are in `include/note/units.hpp`. They are simple `constexpr` wrappers around `int32_t` with converting constructors:

```cpp
struct Minutes {
    int32_t count = 0;
    constexpr Minutes(int32_t m) : count(m) {}
    constexpr Minutes(Hours h) : count(h.count * 60) {}
    constexpr Minutes(Days d) : count(d.count * 1440) {}
    constexpr operator int32_t() const { return count; }
};
```

The `operator int32_t()` means the JSON builder sees a plain integer — no special serialization needed. The voltage-variable builder is in `include/note/voltage_variable.hpp`.

See [examples/hub-configuration/](../examples/hub-configuration/) for working examples.
