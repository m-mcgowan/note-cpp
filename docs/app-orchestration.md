# App Orchestration Design

How `note-cpp-app` hides operational complexity — template lifecycle, sync
direction, packet limits, NTN prerequisites — behind typed, ergonomic APIs.

## Motivation

Real Notecard applications involve multi-step setup sequences where order
matters, constraints vary by radio technology, and mistakes are silent. The
[Satellite Best Practices](https://dev.blues.io/starnote/satellite-best-practices/)
guide alone lists 10+ operational details that every NTN application must get
right. Many of these concerns also apply to cellular and WiFi — NTN just
raises the stakes.

| Concern | Cellular | NTN | Library role |
|---------|----------|-----|-------------|
| Template registration | Performance optimization | Required — unregistered notes silently dropped | Ensure registration before first `note.add` |
| Compact format + port | N/A | Required | Enforced when NTN target configured |
| Packet size limits | None | 340 B (Iridium) / 256 B (Skylo) | Compile-time or runtime validation |
| Directional sync | `hub.sync` does both | Must call `out:true` and `in:true` separately | Transparent direction splitting |
| Initial cellular sync | N/A | Required before any NTN comms | Setup procedure gates on this |
| Sync cadence | Nice to have | Critical for data cost | Type-safe intervals (`Hours`, `Days`) |
| Fixed location | GPS optimization | Avoids slow satellite acquisition | Part of setup configuration |
| Inbound polling cost | Free | ~50 bytes per check | Budget tracking / warnings |

The app layer composes `note-cpp`'s typed API, channels, and state store into
higher-level patterns that handle the ceremony automatically.

---

## Template Manager

The TemplateManager owns the lifecycle of Notecard templates:

1. **Declaration** — app registers templates at init with schema structs
2. **Registration** — `note.template` sent on first `setup()`, tracked in StateStore
3. **NTN validation** — compact format, port number, and packet size enforced
4. **Guard** — `note.add` through the manager ensures the template is registered

```cpp
struct Readings {
    float temp;
    float humidity;
    NOTE_FIELDS(temp, humidity);
};

// Declaration (compile-time)
auto tmpl = note::app::Template<Readings>("readings.qo")
    .port(55)              // required for NTN, validated at registration
    .compact();            // format:"compact" — NTN requirement

// Registration happens inside setup()
// Packet size checked against provider limit (340 B / 256 B)
```

For cellular-only apps, `port` and `compact` are optional. The manager still
handles register-once semantics and re-registration after factory reset.

---

## Sync Manager

The SyncManager abstracts sync direction and cadence:

- **Cellular**: `hub.sync` performs both directions in one call
- **NTN**: must issue separate `hub.sync` with `"out":true` and `"in":true`
- The manager detects the transport mode and splits automatically

```cpp
// App code is transport-agnostic:
sync.outbound();    // hub.sync {out:true} on NTN, hub.sync {} on cellular
sync.inbound();     // hub.sync {in:true} on NTN, hub.sync {} on cellular
sync.both();        // two calls on NTN, one on cellular
```

### Sync cadence

Intervals are configured with type-safe units. The `hub.set` fields are
`Minutes` on the wire, but accept `Hours` and `Days` through implicit
conversion:

```cpp
using namespace note::literals;

api.hub.set()
    .mode("periodic")
    .outbound(15_mins)       // 15 minutes
    .inbound(7_days)         // 10080 minutes on the wire
    .execute();
```

### Inbound budget tracking (NTN)

Each NTN inbound check costs ~50 bytes. The sync manager can track inbound
poll count and warn or throttle when approaching a budget:

```cpp
sync.set_inbound_budget(100);        // max 100 polls per billing period
sync.inbound();                       // decrements budget, warns at threshold
auto remaining = sync.inbound_remaining();
```

---

## Connection Setup

### Initial cellular requirement (NTN)

All NTN products require an initial sync over cellular/WiFi before satellite
communication works. This syncs template definitions and device configuration.
The setup procedure gates on this automatically.

### Fixed location

For stationary deployments (common with NTN), setting a fixed location skips
GPS satellite acquisition:

```cpp
api.card.locationMode()
    .mode("fixed")
    .lat(42.565)
    .lon(-70.783)
    .execute();
```

### Starnote pairing

A Starnote pairs exclusively with one Notecard. Re-pairing requires
`ntn.reset` on the old device, then a cellular sync on the new one. The
manager can track pairing state and guide the re-pair sequence.

---

## Composed Setup Procedure

Uses the `procedure` pattern from `note-cpp-app`:

```cpp
auto setup = note::app::Setup(ch)
    .product("com.example.weather")
    .mode("periodic")
    .outbound(15_mins)
    .inbound(7_days)
    .fixed_location(42.565, -70.783)
    .ntn()
    .template_("readings.qo", note::template_of<Readings>(), {.port = 55});

auto result = setup.run();
```

Internally sequences:
1. `hub.set` — mode, product, sync intervals
2. `note.template` — for each declared template (with NTN validation)
3. `hub.sync` — initial sync (blocks until complete for NTN)
4. `card.location.mode` — if fixed location configured
5. `ntn.status` — verify NTN readiness

Each step checks the previous result. On failure, the procedure returns the
specific step's error with full `ErrorInfo` context.

---

## Examples

### 1. Basic cellular app

The simplest case — periodic sync with a typed template:

```cpp
#include <note/notecard.hpp>
#include <note/api_context.hpp>

using namespace note::literals;

struct Readings {
    float temp;
    float humidity;
    NOTE_FIELDS(temp, humidity);
};

int main() {
    // ... transport setup ...
    note::Api api(nc);

    api.hub.set()
        .product("com.example.weather")
        .mode("periodic")
        .outbound(15_mins)
        .inbound(4_hours)
        .execute();

    api.note.template_().set("readings.qo")
        .body(note::template_of<Readings>())
        .execute();

    // Send data
    api.note.add()
        .file("readings.qo")
        .body(Readings{.temp = 22.5f, .humidity = 65.0f})
        .execute();
}
```

### 2. NTN satellite app — manual ceremony

What a developer writes today without the app layer:

```cpp
#include <note/notecard.hpp>
#include <note/api_context.hpp>

using namespace note::literals;

struct Readings {
    float temp;
    float humidity;
    NOTE_FIELDS(temp, humidity);
};

int main() {
    // ... transport setup ...
    note::Api api(nc);

    // Step 1: Configure hub
    api.hub.set()
        .product("com.example.weather")
        .mode("periodic")
        .outbound(15_mins)
        .inbound(7_days)         // weekly inbound check (~50 bytes each)
        .execute();

    // Step 2: Register compact template with port
    // NTN requires "format":"compact" and "port":1-100.
    // Without this, notes are silently dropped.
    api.note.template_().set("readings.qo")
        .body(note::template_of<Readings>())
        .extra("format", "compact")
        .extra("port", 55)
        .execute();

    // Step 3: Fix location (skip slow GPS acquisition)
    api.card.locationMode().set()
        .mode("fixed")
        .lat(42.565)
        .lon(-70.783)
        .execute();

    // Step 4: Initial cellular sync (required before NTN works)
    api.hub.sync().execute();
    // Must poll hub.sync.status until "completed" ...

    // Step 5: Send data
    api.note.add()
        .file("readings.qo")
        .body(Readings{.temp = 22.5f, .humidity = 65.0f})
        .execute();

    // Step 6: Trigger outbound sync (NTN needs explicit direction)
    api.hub.sync().out(true).execute();

    // Step 7: Later, check for inbound (costs ~50 bytes!)
    api.hub.sync().in(true).execute();
}
```

### 3. NTN satellite app — with app layer

Same functionality, ceremony hidden:

```cpp
#include <note/app/setup.hpp>

using namespace note::literals;

struct Readings {
    float temp;
    float humidity;
    NOTE_FIELDS(temp, humidity);
};

int main() {
    // ... transport setup ...
    note::app::DirectChannel ch(nc);

    auto app = note::app::Setup(ch)
        .product("com.example.weather")
        .mode("periodic")
        .outbound(15_mins)
        .inbound(7_days)
        .fixed_location(42.565, -70.783)
        .ntn()
        .template_("readings.qo", note::template_of<Readings>(), {
            .port = 55,
        });

    // Single call: hub.set → note.template → hub.sync → card.location.mode
    // Validates compact format, checks packet size, waits for initial sync.
    auto result = app.run();
    if (!result) {
        printf("Setup failed: %s\n", to_string(result.error()).c_str());
        return 1;
    }

    // Send data — same API regardless of NTN or cellular
    api.note.add()
        .file("readings.qo")
        .body(Readings{.temp = 22.5f, .humidity = 65.0f})
        .execute();

    // Sync — handles directional split for NTN transparently
    app.sync_outbound();
}
```

### 4. Type-safe intervals across the API

Duration conversions work everywhere, not just `hub.set`:

```cpp
using namespace note::literals;

// hub.set — outbound/inbound are Minutes fields
api.hub.set()
    .outbound(15_mins)        // Minutes literal
    .inbound(7_days)          // Days → Minutes (10080)
    .execute();

// card.attn — seconds field accepts Minutes/Hours
api.card.attn()
    .mode("arm")
    .seconds(5_mins)          // Minutes → Seconds (300)
    .execute();

// card.sleep — express naturally
api.card.sleep()
    .seconds(12_hours)        // Hours → Seconds (43200)
    .execute();

// Voltage-variable sync cadence with mixed units
auto req = api.hub.set();
req.mode = "periodic";
req.voutbound.usb(5_mins).high(15_mins).normal(1_hours).low(4_hours).dead(0);
req.vinbound.usb(30_mins).high(2_hours).normal(12_hours).low(7_days).dead(0);
req.execute();

// Compile-time safety:
// api.hub.set().outbound(300_s);  // error: Seconds cannot implicitly convert to Minutes
```

### 5. Environment config with intervals

The `ConfigManager` (designed in `note-cpp-app.md`) uses the same units:

```cpp
struct AppConfig {
    note::Seconds idle_interval = 60_s;
    note::Minutes sync_interval = 15_mins;
    note::Days    inbound_check = 7_days;
    bool          developer_mode = false;
};

template<>
struct note::EnvSchema<AppConfig> {
    static constexpr auto fields = std::tuple{
        note::field("idle interval",  &AppConfig::idle_interval).unit<Seconds>(),
        note::field("sync interval",  &AppConfig::sync_interval).unit<Minutes>(),
        note::field("inbound check",  &AppConfig::inbound_check).unit<Days>(),
        note::field("developer mode", &AppConfig::developer_mode),
    };
};
```

Environment strings with unit suffixes are parsed automatically:
```
"idle interval" = "3 mins"   → 180 seconds
"idle interval" = "90"       → 90 seconds (field's .unit<Seconds>() applies)
"inbound check" = "2"        → 2 days
```

---

## Relationship to existing design

This document extends `docs/note-cpp-app.md`:
- **TemplateManager** (designed there) gains NTN-specific validation
- **SyncManager** (designed there) gains directional sync awareness
- **Setup** is a new composed procedure that orchestrates multiple managers
- **Type-safe units** are in `note-cpp` core (`include/note/units.hpp`)
- **Channel** and **StateStore** are unchanged — they remain the execution and state primitives
