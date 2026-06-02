# ATTN Pin

The `card.attn` API controls the Notecard's ATTN pin, which can wake a sleeping host MCU or interrupt an awake one when events occur (file changes, connectivity, motion, etc.).

## Quick reference

```cpp
#include <note/api.hpp>

note::Api nc(notecard);

// Arm for file changes with 2-minute timeout
nc.card.attn().arm(note::attn::files).seconds(120).execute();

// Re-arm after ATTN fires (idempotent — safe to call repeatedly)
nc.card.attn().rearm(note::attn::files).seconds(120).execute();

// Disarm all triggers
nc.card.attn().disarm().execute();

// Query current state
auto rsp = nc.card.attn().query().execute();
```

## Operation types

Each `card.attn` mode has a dedicated operation type — see [Focused operations](../../using-the-api.md#focused-operations-on-multi-purpose-requests) for the broader pattern. The factory method builds the
correct wire format automatically.

| Factory method | Wire mode | Purpose |
|---------------|-----------|---------|
| `.arm(triggers)` | `arm,<triggers>` | Arm ATTN for specific event triggers |
| `.rearm(triggers)` | `rearm,<triggers>` | Idempotent arm — re-enables without briefly disarming |
| `.disarm()` | `disarm,-all` | Clear all triggers, pull ATTN HIGH |
| `.watchdog()` | `watchdog` | Watchdog timer (host must pet before timeout) |
| `.sleep()` | `sleep` | MCU sleep with optional payload storage |
| `.retrieve()` | (start=true) | Retrieve payload after sleep wake |
| `.query()` | (no mode) | Read current ATTN configuration |
| `.off()` | (off=true) | Disable all ATTN processing |
| `.on()` | (on=true) | Re-enable ATTN processing after `.off()` |

## Trigger flags

Trigger flags control which events fire the ATTN pin. Use named methods or
flag constants:

```cpp
// Named methods (fluent chaining)
nc.card.attn().arm()
    .connected()
    .files()
    .motion()
    .seconds(300)
    .execute();

// Flag constants (combinable with |)
using namespace note::attn;
nc.card.attn().arm(connected | files | motion)
    .seconds(300)
    .execute();
```

Available trigger flags:

| Flag | Named method | Fires when... |
|------|-------------|---------------|
| `note::attn::auxgpio` | `.auxgpio()` | AUX GPIO input changes |
| `note::attn::connected` | `.connected()` | Notecard connects to cellular |
| `note::attn::env` | `.env()` | Environment variable changes |
| `note::attn::files` | `.files()` | Watched Notefiles are modified |
| `note::attn::location` | `.location()` | GPS position fix acquired |
| `note::attn::motion` | `.motion()` | Accelerometer detects motion |
| `note::attn::motionchange` | `.motionchange()` | Motion status changes (moving/stopped) |
| `note::attn::signal` | `.signal()` | Signal received from Notehub |
| `note::attn::usb` | `.usb()` | USB power state changes |
| `note::attn::wireless` | `.wireless()` | Wireless status changes |

## arm() vs rearm()

- **`arm()`** clears pending events and arms. If already armed, it briefly
  disarms (ATTN goes HIGH) then re-arms. This can cause a spurious wake if
  the host is watching for ATTN edges.

- **`rearm()`** is idempotent. If already armed, it resets mode/files/seconds
  to the new values without disarming first. Safe to call every time ATTN fires
  in your interrupt handler.

Typical interrupt loop:

```cpp
void on_attn_fired() {
    // Process the event...

    // Re-arm with same triggers for next event
    nc.card.attn().rearm(note::attn::files | note::attn::connected)
        .seconds(300)
        .execute();
}
```

## Watching specific files

The `files` trigger fires when any of the named Notefiles are modified.
Specify which files to watch:

```cpp
nc.card.attn().arm(note::attn::files)
    .files().add("data.qi")
    .files().add("config.db")
    .seconds(600)
    .execute();
```

## Sleep with payload

The host MCU can store a payload in Notecard memory before sleeping, then
retrieve it after waking:

```cpp
// Sleep for 60 seconds, store state
nc.card.attn().sleep()
    .seconds(60)
    .payload("my_state_data")
    .command();   // fire-and-forget (MCU is about to sleep)

// ... MCU sleeps, ATTN fires after 60s ...

// Retrieve stored payload after wake
auto rsp = nc.card.attn().retrieve().execute();
if (rsp) {
    auto payload = rsp.payload;  // "my_state_data"
}
```

## Disable ATTN processing

To completely stop ATTN monitoring (not just clear triggers):

```cpp
nc.card.attn().off().execute();   // disable
nc.card.attn().on().execute();    // re-enable
```

Unlike `disarm()` which clears triggers but leaves processing enabled,
`off()` stops all ATTN processing. The setting is retained across restarts.

## Raw request escape hatch

For modes or combinations not covered by the operation types, use the base
`Request` type with the mode string directly:

```cpp
note::api::CardAttn::Request req;
req.mode = "arm,connected,files";
req.seconds = 120;
nc.execute(req);
```

String literals in the mode field are validated at compile time (C++20) —
typos like `"conected"` produce a compile error.
