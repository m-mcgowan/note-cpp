# Changelog

<!--
All notable changes to this project will be documented in this file.
Follows [Keep a Changelog](https://keepachangelog.com/) conventions.

Write entries from the library user's perspective. Internal implementation
details belong in git commit messages and design docs, not here.
-->

## [Unreleased]

## [0.3.0] - 2026-07-02

### Added
- [I2C shared-bus & multi-threaded safety](docs/transport-i2c.md) — opt-in `NOTE_I2C_BUS_LOCK` with an `IBusLock` seam and a ready-made `FreeRtosBusLock` adapter. `exclusive()` and `keep_ready()` group several exchanges into one atomic operation (with optional RTX/CTX readiness signalling), so a shared I²C bus can be driven safely from multiple tasks. ([`429458b`](https://github.com/m-mcgowan/note-cpp/commit/429458b), [`ae53234`](https://github.com/m-mcgowan/note-cpp/commit/ae53234))
- [`nc.ping()`](docs/troubleshooting.md#im-getting-no-response-from-the-notecard) — one-shot echo connectivity probe (single attempt, short timeout, no retry/CRC/reset); safe before any other transaction. Also on `StaticNotecard`. ([`65217f8`](https://github.com/m-mcgowan/note-cpp/commit/65217f8))
- [Compile-time `body_template`](docs/body-values.md) — build request bodies (nested objects and arrays) at compile time, emitted as either JSON text or JSONB. ([`b8be3dc`](https://github.com/m-mcgowan/note-cpp/commit/b8be3dc))
- Raw JSON body literals now work under `NOTE_JSONB`, re-encoded to binary opcodes via SAX replay. ([`cd94afa`](https://github.com/m-mcgowan/note-cpp/commit/cd94afa))
- [First-class memory control](docs/memory.md) — `HeapResetPool` and the `NOTE_NO_RESPONSE_RAII` opt-out for arena-only builds. ([`72a5d12`](https://github.com/m-mcgowan/note-cpp/commit/72a5d12), [`420094e`](https://github.com/m-mcgowan/note-cpp/commit/420094e))

### Changed
- [Streaming is now the primary presentation](docs/streaming-and-tree.md): responses SAX-stream into typed sinks by default, with the buffered JSON-tree path as the fallback. The `NOTE_NO_BUFFERED` flag was renamed to [`NOTE_NO_JSON_TREE`](docs/feature-flags.md); the old spelling is honoured as a deprecated alias. ([`92bb655`](https://github.com/m-mcgowan/note-cpp/commit/92bb655))
- [Wire format and parse strategy are now orthogonal](docs/jsonb.md) — JSONB (binary) vs JSON (text) is chosen independently of streaming vs tree parsing. ([`e1d3c72`](https://github.com/m-mcgowan/note-cpp/commit/e1d3c72))

### Fixed
- Singleton (`NOTE_SINGLETON`) correctness: the streaming-parse flag is set on the singleton path, Notecard error messages get allocator-backed lifetime (no longer dangle past `execute()`), and safety / request IDs / array fields are plumbed through the singleton thunks. ([`7b7664b`](https://github.com/m-mcgowan/note-cpp/commit/7b7664b), [`d8006d4`](https://github.com/m-mcgowan/note-cpp/commit/d8006d4), [`9985116`](https://github.com/m-mcgowan/note-cpp/commit/9985116))

### Internal
- Testing: JSON/JSONB parser fuzzing under ASan/UBSan added to the release gate ([`b69854f`](https://github.com/m-mcgowan/note-cpp/commit/b69854f)); an on-demand [mutation-testing harness](tools/mutation-testing/README.md) (`ci.sh --mutate`) that drove new assertions into the COBS, retry, JSONB, and dispatch tests ([`cb20035`](https://github.com/m-mcgowan/note-cpp/commit/cb20035), [`6dee91a`](https://github.com/m-mcgowan/note-cpp/commit/6dee91a)); a hardware-in-the-loop sweep over ESP32-S3 serial + I²C ([`17f95a2`](https://github.com/m-mcgowan/note-cpp/commit/17f95a2)); device coverage extended to the I²C interface ([`b5abf7b`](https://github.com/m-mcgowan/note-cpp/commit/b5abf7b)).
- Refactoring: `Notecard::execute()`'s type-independent prologue collapsed into a non-template core ([`525dd4b`](https://github.com/m-mcgowan/note-cpp/commit/525dd4b)); a re-entrant `run_operation` chokepoint underpins the operation lock ([`73d1b45`](https://github.com/m-mcgowan/note-cpp/commit/73d1b45)).

## [0.2.0] - 2026-04-21

### Added
- [JSONB binary wire format](docs/jsonb.md) — opt-in via `NOTE_JSONB`, replaces JSON text with compact binary opcodes
- [`note::scan` / `JsonView`](docs/platforms/arduino/guide.md#binary-size-comparison) — SAX-free JSON field extraction; drops AVR flash to 10,882 B (−14 KB vs note-c)
- `FlashString` + `F()` overloads — scan keys held in flash/PROGMEM on AVR
- `ErrorMessage` with PROGMEM error enum names — net flash/RAM win on AVR
- [`has_value()`](docs/working-with-responses.md#checking-for-fields) on response fields
- [`.into(T&)`](docs/working-with-responses.md#typed-body-parsing-recommended) on request builders for streaming body parse
- [`Response::max_arena_size`](docs/arena-sizing.md) for compile-time arena budgets
- 64-bit JSON integers (`int64_t`) with schema-driven narrowing via `json_time_t` and friends
- `c_str()` on response string types — interned strings are null-terminated
- `note.hpp` brings the `note::` namespace into scope by default
- Arduino `Printable` support for response fields and errors
- [Raw JSON AVR benchmark](docs/platforms/arduino/guide.md#binary-size-comparison) in `tools/binary-size-comparison/`
- Feature flags: [`NOTE_MINIMAL`](docs/feature-flags.md), [`NOTE_NO_API_GROUPS`](docs/feature-flags.md), `NOTE_NO_RETRY`, `NOTE_NO_REQUEST_IDS`, `NOTE_UNICODE_ESCAPES`
- [API layers guide](docs/using-the-api.md#the-three-layers) — typed API → base requests → lambda builder → raw JSON
- [note-c bridge pattern](docs/platforms/arduino/migration-from-note-arduino.md) for gradual migration

### Changed
- Examples reorganized to platform-first layout: `examples/arduino/`, `examples/stdcpp/`. Tests moved to `tests/`; binary-size comparison moved to `tools/`
- Endpoint API uses the nested form only (e.g. `nc.card.location.mode`, `nc.card.aux.serial`, `nc.card.binary.status`)
- Response string types settled on `c_str()` + `printable()` pattern

### Fixed
- Unqualified `Notecard` on Arduino now resolves to `note::arduino::Notecard` without ambiguity (previously required `#define NOTE_USING_NAMESPACE 0`). Qualify as `note::Notecard` when you need the transport-agnostic host class.
- Coexistence with the legacy `note-arduino` (Blues) library — `Notecard` symbol no longer collides
- `examples/arduino/i2c_basic.ino` AVR include path uses `<nonstd-lite.hpp>` from `nonstd-lite-bundle`
- Lazy `Serial.begin` in `SerialHal` — Wokwi Uno `API_STYLE=4` now runs
- GCC 13/14 C++23 builds pass clean (`-Werror`)

### Internal
- Unified SAX dispatch (704 B flash saved on AVR)
- Static thunks eliminate per-endpoint lambda duplication (1,324 B flash saved on AVR)
- Branch coverage 89% → 96.2%; lcov 2.3; all 5 compiler variants green
- CI: split workflow into distinct steps per stage; run `--full` on build matrix
- AVR: 26,488 flash (82%), 832 RAM (41%), zero heap

## [0.1.0] - 2026-03-29

### Added
- 74 auto-generated endpoint types with [fluent builder API](docs/using-the-api.md#calling-styles-within-the-typed-layer)
- [`ApiResult<T>`](docs/working-with-responses.md) dot-access for response fields
- Native [serial](docs/transport-serial.md) and [I2C](docs/transport-i2c.md) transport with CRC32, retry, and chunking
- [Type-safe duration units](docs/duration-units.md): `Seconds`, `Minutes`, `Hours`
- [`VoltageVariable`](docs/internal/custom-field-transforms.md) builder for structured voltage thresholds
- [`JsonBuf`](docs/body-values.md) constexpr JSON builder; [`json_fmt`](docs/body-values.md) compile-time validated templates (C++20)
- [Target constraints](examples/stdcpp/target-filtering.cpp) via `make_api()` with `target<Product>()` / `target<Rat>()`
- [`body()`](docs/body-values.md) helper and `NOTE_FIELDS` macro for typed request/response bodies
- Arduino `Printable` support for response fields and errors
- [Raw JSON escape hatch](docs/using-the-api.md#escape-hatches): `transact(json, buf)` and `send(json)`
- [Examples](examples/): getting started, sending notes, hub configuration, attention, location

### Fixed
- `set_body()` ambiguity between typed and untyped overloads
