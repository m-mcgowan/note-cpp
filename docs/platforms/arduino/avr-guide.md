# AVR Guide

> **Draft.** Mat: TODOs welcome inline. This is the missing "how do I use note-cpp on a 32 KB / 2 KB part?" doc. Working example: [`tools/binary-size-comparison/src/main_avr_notecpp.cpp`](../../../tools/binary-size-comparison/src/main_avr_notecpp.cpp).

note-cpp targets AVR (ATmega328P, ATmega2560) as a first-class platform. The library was sized to leave usable room for application code on a Uno-class board (32 KB flash, 2 KB RAM) — but you have to opt into the right idioms. This guide covers the trade-offs.

## Constraints to plan for

| Resource | ATmega328P (Uno) | ATmega2560 (Mega) |
|---|---|---|
| Flash | 32 KB | 256 KB |
| SRAM | 2 KB | 8 KB |
| EEPROM | 1 KB | 4 KB |
| UARTs | 1 | 4 |

- **No heap.** note-cpp on AVR uses only static + stack memory. The library does not call `malloc`/`new`. You must do the same — every accidental `String`, `std::string`, or `new` pulls the dynamic allocator into your firmware.
- **Stack budget is small.** Loop task starts with ~8 KB on ESP32 but only the SRAM total on AVR — be careful with deep response structs.
- **String literals default to `.data` (RAM).** Without `F(...)` or `PROGMEM`, every `"hello"` costs RAM. See [PROGMEM strings](#progmem-strings) below.

## Choose your API style

note-cpp exposes the same Notecard requests through several layers, each with a different size/DX trade-off. On a 32 KB part you'll usually pick one and stay with it.

| Style | Binary cost | Ergonomics | When to use |
|---|---|---|---|
| **1. API groups** — `api.card.temp().read().execute()` | largest | best | You have flash to spare (Mega, ESP32). Typed responses, IntelliSense-friendly. |
| **2. Direct types** — `nc.execute(note::api::CardTemp::Read{})` | medium | good | A few hundred bytes smaller — same typed responses, no group factory chains. |
| **3. Raw + SAX sink** — `transact_dispatch` + custom `JsonSink` | smaller flash, smallest RAM | low | You need the lowest RAM footprint and only a handful of fields. The SAX parser pulls in ~8 KB flash, but no response buffer. |
| **4. Raw + JsonView** — `transact_raw` + `JsonView` scan | smallest flash | low | You need the lowest flash. No SAX parser; you provide a response buffer and scan for known fields. |

### Worked example — measured sizes

The matrix below comes from [`tools/binary-size-comparison/src/main_avr_notecpp.cpp`](../../../tools/binary-size-comparison/src/main_avr_notecpp.cpp), an 8-endpoint app (configure hub, define template, read sensors, publish, check status, read voltage, read inbound notes, read env vars). Set `-DAPI_STYLE=N` to pick a style; each `pio run -e avr-notecpp-<env>` produces one row.

| Style | Env | Flash | % flash | RAM | Notes |
|---|---|---|---|---|---|
| 1 — API groups | `avr-notecpp` | 25,198 B | 78.1% | 773 B | Most ergonomic; `api.hub.set().product(...).execute()` |
| 2 — Direct types | `avr-notecpp-direct` | 24,788 B | 76.8% | 753 B | Same response parser as style 1, no group factory chains |
| 3 — Raw + SAX sink | `avr-notecpp-raw` | 21,172 B | 65.6% | 781 B | Pulls in SAX parser; no response buffer |
| 4 — Raw + JsonView | `avr-notecpp-scan` | 11,322 B | 35.1% | 695 B | No SAX parser; you own the response buffer |
| (ref) note-c | `avr-notec` | 25,076 B | 77.7% | 729 B + 371 B heap = 1,100 B | Comparison baseline |

Reproduce: `pio run -d tools/binary-size-comparison -e avr-notecpp-direct -e avr-notecpp-raw -e avr-notecpp-scan -e avr-notec`. The build flags to enable each style are `-DAPI_STYLE=1..4`.

## Sizing the arena

For styles 1–2, the response parser interns string fields into an arena you provide. Use `StaticArena<RequestSetT>` to size the arena automatically from the requests you actually use:

```cpp
using UsedRequests = note::RequestSet<
    note::api::HubSet,
    note::api::NoteTemplate::Define,
    note::api::CardTemp::Read,
    note::api::NoteAdd,
    note::api::CardStatus,
    note::api::CardVoltage::Read,
    note::api::NoteGet::Read,
    note::api::EnvGet
>;
static note::StaticArena<UsedRequests> arena;

using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
static SerialNotecard nc(note::arena_allocator(arena), Serial, 9600);
static note::Api api(nc);
```

`StaticArena<R>::size` is `R::max_arena_size` — the maximum any single response in the set can need. Reset between transactions:

```cpp
void loop() {
    arena.reset();
    auto temp = api.card.temp().read().execute();
    // ...
}
```

See [arena-sizing.md](../../arena-sizing.md) for the underlying mechanism and how `RequestSet::max_arena_size` is computed.

For styles 3–4 (raw JSON), there is no arena — you pass a `char rsp[N]` buffer directly to `transact_raw` and either scan it with `JsonView` or pipe it through a SAX sink.

## Raw-JSON ergonomics — `nc.transact_raw(...)`

The raw-JSON path (styles 3–4) is the smallest-flash escape hatch. The shape is intentionally explicit — no parser, no arena, no allocator — but `StaticNotecard` provides a few overloads that collapse most of the boilerplate. All three accept the same response buffer + optional timeout (default 10 s):

```cpp
// Build a request — JsonBuf<N> for runtime, note::json<...>() for compile-time.
note::JsonBuf<64> req;
req.add("req", "card.temp");

// Send it. .view() and sizeof(rsp) are deduced; default 10 s timeout.
char rsp[64];
auto v = note::JsonView(nc.transact_raw(req, rsp));   // unwraps Result<string_view>
float temperature = v.get_float(K("value"));
```

The forwarders cost zero flash — they inline straight to the underlying `transport.transact_raw(...)`. You can reach the verbose form (`nc.stack().transport.transact_raw(view, ptr, size, timeout)`) directly if you need an unusual timeout or are doing something the forwarders don't cover.

### Compile-time pre-built requests

When the request is fully static, build it once with `note::json<...>()` and the JSON lives in `.rodata` — zero RAM cost per call:

```cpp
constexpr auto temp_req = note::json<[](auto& b){ b.add("req","card.temp"); }>();

void loop() {
    char rsp[64];
    auto v = note::JsonView(nc.transact_raw(temp_req, rsp));
    float temperature = v.get_float(K("value"));
    // ...
}
```

## PROGMEM strings

On AVR, every string literal you write is RAM-resident unless you put it in flash explicitly. note-cpp provides flash-aware overloads at the relevant API surfaces.

### Generated request strings — already in PROGMEM

The codegen marks request name constants with `NOTE_FLASH_ATTR`, so `note::api::CardTemp::req` etc. don't cost RAM.

### Your own constants — `F()` for one-off, `NOTE_FLASH_ATTR` for arrays

For literals you write inside your firmware, the standard Arduino `F("...")` macro routes the string into PROGMEM. Most note-cpp APIs that take a key (e.g. `JsonView::get_float`) accept `F(...)` directly via the `FlashString` implicit conversion:

```cpp
float t = view.get_float(F("value"));
```

For arrays of strings (e.g. flag tables), use `NOTE_FLASH_ATTR` and wrap with `note::flash(...)` at the call site:

```cpp
NOTE_FLASH_ATTR const char k_temp[]  = "temperature";
NOTE_FLASH_ATTR const char k_humid[] = "humidity";
// ...
v.get_float(note::flash(k_temp), 0.0f);
```

The internal mechanics (and the trade-off vs raw `note::flash("literal")`) are documented in the contributor doc [`docs/internal/avr-flash-strings.md`](../../internal/avr-flash-strings.md).

### `K()` — the project-local toggle pattern

For maximum flash savings you want `F(...)` on every key. But `F(...)` returns `__FlashStringHelper*`, which is Arduino-specific — code that has to compile on host or ESP32 needs a fallback. The convention used in our example is a local macro:

```cpp
#if defined(USE_FLASH_KEYS) && USE_FLASH_KEYS
  #define K(s) F(s)
#else
  #define K(s) s
#endif

float t = view.get_float(K("temperature"));
```

This is **not** a library macro — copy it into your project if you want the same toggle.

## Static Notecard, no heap

`note::Notecard` (the dynamic version) reaches for a small dynamic allocator. On AVR you want `note::StaticNotecard<Stack>`, which wires the transport stack at compile time.

```cpp
using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
static SerialNotecard nc(note::arena_allocator(arena), Serial, 9600);
```

`StaticNotecard<Stack>` is non-virtual — every transport call resolves to a direct call on the concrete stack type. No vtables, no dispatch indirection.

### `NOTE_SINGLETON` — already on for AVR

When `NOTE_MINIMAL=1` is defined (the typical AVR config), `NOTE_SINGLETON=1` is set automatically. You don't have to do anything — but it's worth understanding what it does, because it's the reason you can't have two `Api` instances on the same firmware.

**What it changes.** With `NOTE_SINGLETON=0`, every group factory (`api.card`, `api.hub`, etc.) carries a `Notecard*` member, and `Api`'s constructor walks ~28 of them setting that pointer. With `NOTE_SINGLETON=1`, that pointer is a **class-static** instead — one slot shared by all factories, set once when `Api<NcT>` is constructed. The factory members themselves become empty.

**What it costs you.** Only one `Notecard` per binary. If you do `Api a1(nc1); Api a2(nc2);`, `a2`'s constructor overwrites `a1`'s static pointer and `a1` silently breaks. On AVR with one Notecard on Serial, this is never an issue — but if you ever need multi-Notecard, override the default:

```ini
build_flags = -DNOTE_MINIMAL=1 -DNOTE_SINGLETON=0
```

**What it saves.** On the 8-endpoint AVR app, ~120 B flash. RAM is unchanged for this app — but on bigger apps the savings on `sizeof(Api<>)` add up.

| Build | Flash | RAM |
|---|---|---|
| `NOTE_MINIMAL=1` (default → `NOTE_SINGLETON=1`) | 24,788 B | 753 B |
| `NOTE_MINIMAL=1 -DNOTE_SINGLETON=0` | 24,910 B | 753 B |

The flag is independent of `StaticNotecard` vs `Notecard` — both work either way. `StaticNotecard` removes virtual-dispatch overhead at the transport layer; `NOTE_SINGLETON` removes per-instance pointer state at the API layer. Combined, they're the AVR-default config.

## Opting out of features you don't use

Each opt-out is a flash savings of a few hundred bytes to a few KB.

| Flag | What it disables | When to set |
|---|---|---|
| `NOTE_MINIMAL` | The full `Api<>` group surface; keeps direct types only | When using styles 2–4 |
| `NOTE_NO_RETRY` | Transport retry logic | When you handle errors manually |
| `NOTE_NO_REQUEST_IDS` | Request-ID auto-increment + matching | Single-request-at-a-time apps |
| `NOTE_ARDUINO_NO_WIRE` | I2C support (avoids `<Wire.h>`) | Serial-only Notecard |

See [feature-flags.md](../../feature-flags.md) for the complete list.

## Common pitfalls

- **`Wire.h` pulled in unintentionally.** Even if you're Serial-only, `<note/arduino.hpp>` auto-includes `<Wire.h>` via `__has_include` unless you `#define NOTE_ARDUINO_NO_WIRE 1` before the include. Link-time GC drops most of it, but the source-level opt-out is the only hard guarantee.
- **`String` accidentally instantiated.** Arduino's `String` class allocates on the heap. If something pulls it in (e.g. via `Serial.print(int)` returning a `size_t` you store in a `String`), you've doubled your flash budget.
- **`new`/`malloc` reachability.** Compiling with `-Wl,-Map=firmware.map` and grepping for `__cxa_pure_virtual`, `_malloc_r`, or `operator new` will tell you if dynamic allocation crept in.
- **Stack overflow into globals.** AVR runs the stack downward from the top of SRAM into your heap (or globals if no heap). A deep response struct or a recursive parser can overwrite globals before SP underflows. If your app suddenly malfunctions during a known-large transaction, suspect this.

## See also

- [Using the API](../../using-the-api.md) — calling styles + typed → lambda → raw layered walk-through (platform-agnostic)
- [Arena sizing](../../arena-sizing.md) — `RequestSet::max_arena_size` mechanics
- [Feature flags](../../feature-flags.md) — full opt-out reference
- [`docs/internal/avr-flash-strings.md`](../../internal/avr-flash-strings.md) — how `FlashString` and the PROGMEM scan path work internally

<!--
Pending: a `JsonBuf` sizing rubric (small body / single request / req+body /
request with body). Blocked on the auto-size proposal; once that lands the
rubric becomes "use this number for X" rather than a manual decision tree.
-->

