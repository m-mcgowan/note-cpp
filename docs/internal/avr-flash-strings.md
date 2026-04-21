# AVR flash vs RAM for string literals

On AVR (Harvard architecture) every `const char*` literal that is
referenced at runtime without `PROGMEM` lands in the `.data` section,
which is **loaded from flash into RAM at startup**. So a 12-byte key
like `"temperature"` costs:

- 12 bytes of flash (the initializer)
- 12 bytes of RAM (the runtime copy)

On constrained Notecard apps (Uno, 2 KB RAM) a handful of key strings
can easily eat 10–20 % of available RAM. This doc captures where the
strings live today, which ones are worth moving, and how the flash-key
API for `note::scan` / `JsonView` is exposed.

## What's in `.data` on AVR today

Measured on `env:avr-notecpp-scan` (ATmega328P):

```
.data = 310 bytes
```

Category breakdown (from `avr-objdump -s -j .data`):

| Category | Examples | ~Bytes |
|---|---|---|
| Transport error names | `send_failed`, `response_lost`, `notecard`, `json`, `not_ready`, `overflow` | ~65 |
| Transport diagnostic | `no response`, `E` | ~15 |
| Request names | `hub.set`, `note.add`, `card.temp`, `note.get`, `env.get`, `card.status`, `card.voltage`, `note.template` | ~80 |
| Field names (sketch scan keys) | `temperature`, `humidity`, `value`, `connected`, `body`, `file`, `product`, `mode`, `outbound` | ~70 |
| User payload | `sensors.qo`, `config.qi`, `com.example.size-test`, `periodic` | ~50 |
| bool formatter | `true`, `false` | ~11 |
| Misc | vtable, small object data | ~20 |

Most of this is user-space literals passed through the library via
`string_view`. Moving any individual category to flash requires either
a PROGMEM-aware API or a migration of the calling code.

## Scan API for flash keys — `v.get_X(note::flash(key), def)`

### Both key types are always available

The scan/view API exposes two parallel key overload sets that coexist
— users never have to pick one or the other up front. Overload
resolution picks the right path based on the argument type at each
call site:

| Call form | Deduced key type | Compare path | `note::flash()` needed? |
|---|---|---|---|
| `v.get_float("temp", 0.0f)` | `string_view` (from `const char[N]`) | RAM byte compare | — |
| `v.get_float(runtime_buf, 0.0f)` | `string_view` (from `const char*`) | RAM byte compare | — |
| `v.get_float(std::string_view{...}, 0.0f)` | `string_view` | RAM byte compare | — |
| `v.get_float(F("temp"), 0.0f)` | `FlashString` (via implicit ctor from `__FlashStringHelper*`) | `pgm_read_byte` on AVR | **No** |
| `v.get_float(note::flash(k_array), 0.0f)` | `note::FlashString` | `pgm_read_byte` on AVR, plain load elsewhere | **Yes** — required for `NOTE_FLASH_ATTR` arrays |
| `v.get_float(note::flash(F("temp")), 0.0f)` | `note::FlashString` | `pgm_read_byte` on AVR | redundant — `F()` direct is cleaner |

Consequences:

- **Runtime-generated keys** (e.g. `snprintf` into a stack buffer)
  Just work through the `string_view` path — no special handling.
- **Forgetting `F()` on Arduino** is not a bug. The literal goes
  through the `string_view` path (RAM compare), which is always
  correct; it just doesn't save RAM.
- **Mixing** flash and RAM keys in the same response extraction is
  fine. Typically you'd use flash keys for known-at-compile-time
  field names and RAM keys for everything else.
- **Non-AVR platforms** (ESP32, SAMD, host, etc.): `NOTE_PROGMEM=0`,
  so the FlashString path collapses to a plain pointer load — same
  generated code as the `string_view` path. There's no penalty for
  using it, but also no benefit.

### API surface

One overload per primitive (`field`, `object`, `array`) and per typed
extractor (`get`, `get_int`, `get_float`, `get_double`, `get_bool`,
`get_str`) on both `note::scan::*` and `note::JsonView`. The
FlashString overloads dispatch to a `matches_at` helper that reads the
key via `pgm_read_byte` on AVR and plain load elsewhere.

### Declaring flash keys

Two idioms. Pick whichever fits your code:

1. **Portable (any Harvard target)** — declare with `NOTE_FLASH_ATTR`:
   ```cpp
   static const char k_temp[] NOTE_FLASH_ATTR = "temperature";
   float t = v.get_float(note::flash(k_temp), 0.0f);
   ```

2. **Arduino ergonomic** — use the `F()` macro:
   ```cpp
   float t = v.get_float(note::flash(F("temperature")), 0.0f);
   ```
   `F()` forces the string into PROGMEM, so `note::flash(F(...))`
   cannot be misused. Arduino-only.

### The one footgun

`note::flash("raw literal")` — a literal passed directly to
`note::flash()` without `NOTE_FLASH_ATTR` or `F()`. On AVR the scan
code will read the wrong bytes (reads flash at the literal's RAM
address → garbage), producing silent missing-key results. On
non-AVR it works fine because PROGMEM is a no-op.

Rule of thumb: never call `note::flash()` on a bare string literal.
Use `F()` on Arduino, or a `NOTE_FLASH_ATTR`-declared array.

### Breakeven

- Fixed cost: one `matches_at(FlashString)` helper (~30–40 B flash),
  paid once across all call sites.
- Per-key win: ~(key_length + 1) B RAM, minus any keys that the rest
  of the program already references through a non-flash API (those
  stay in `.data` regardless).
- Measured on STYLE=4 (ATmega328P, 5 scan keys, 3 shared with
  JsonBuf): **−16 B RAM and −32 B flash** vs the RAM-key baseline
  via `F()` direct form (post error-message migration — the gap
  narrowed after `ErrorMessage` and PROGMEM enum tables landed).

### `always_inline` on the FlashString implicit ctor

`FlashString(const __FlashStringHelper*)` is marked
`__attribute__((always_inline))`. Without it GCC emits the
constructor out-of-line and the flash-key variant grows by ~100 B
compared to the free-function `note::flash(F(...))` path — plausibly
because the compiler's default member-function inlining heuristics
are more conservative than for free functions. Forcing inlining
makes the implicit conversion path strictly the smallest.

### Why not template on key type?

Templating each extractor on a key type (`Key = string_view` vs
`Key = const __FlashStringHelper*`) doubles the number of
instantiations. Explicit overloads keep each code path dedicated and
avoid accidental instantiation on unexpected types.

### Why not a runtime-tagged `Key` struct?

A struct that holds a pointer plus an "in flash?" flag would branch
per byte comparison. The branch cost is small, but flash-only users
then pay for the branch forever. Overloads let the compiler pick the
right path at the call site with zero runtime overhead.

## Transport error strings — TBD

Error strings (`to_string(Error)` in `error.hpp`) currently return
plain `string_view` values that live in `.data`. Migrating them to
PROGMEM-backed storage is a bigger blast radius: the accessor returns
a flash pointer (not a `string_view`), which changes how callers
consume it. Decision pending — revisit once the scan flash-key work
lands.

Options under consideration:

1. **Indexed PROGMEM table** with a `to_flash(Error)` accessor and a
   compatibility `to_string(Error)` that copies into a small scratch
   buffer. Low blast radius; accessor ergonomics differ.
2. **Dual representation** on `ErrorInfo` — keep a flash pointer
   alongside the existing `string_view` and choose at construction
   time. Easier migration but doubles the struct size.
3. **Leave as-is**. RAM impact (~80 B) is real but not catastrophic;
   app-layer literals dominate the `.data` budget on STYLE=4.

## Related helpers already in the codebase

- `note/progmem.hpp` — `NOTE_FLASH_ATTR`, `NOTE_PROGMEM` wrappers
- `note/field_desc.hpp` — `detail::read_field_desc` (reads a PROGMEM
  struct), `detail::flash_key_eq` (compares a `string_view` against a
  PROGMEM C string)

The scan flash-key overloads should reuse `flash_key_eq`-style logic
to avoid a duplicate byte-compare routine.
