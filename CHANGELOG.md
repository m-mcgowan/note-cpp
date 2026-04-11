# Changelog

<!--
All notable changes to this project will be documented in this file.
Follows [Keep a Changelog](https://keepachangelog.com/) conventions.

Write entries from the library user's perspective. Internal implementation
details belong in git commit messages and design docs, not here.
-->

## [Unreleased]

### Added
- [JSONB binary wire format](docs/jsonb.md) — opt-in via `NOTE_JSONB`, replaces JSON text with compact binary opcodes
- [`has_value()`](docs/working-with-responses.md#checking-for-fields) on response fields
- [`.into(T&)`](docs/working-with-responses.md#typed-body-parsing-recommended) on request builders for streaming body parse
- [`Response::max_arena_size`](docs/arena-sizing.md) for compile-time arena budgets
- Feature flags: [`NOTE_MINIMAL`](docs/feature-flags.md), [`NOTE_NO_API_GROUPS`](docs/feature-flags.md) for constrained targets

### Fixed
- GCC 13/14 C++23 builds now pass clean (`-Werror`)

### Internal
- Unified SAX dispatch (704B flash saved on AVR)
- Static thunks eliminate per-endpoint lambda duplication (1,324B flash saved on AVR)
- CI: upgraded to lcov 2.3; all 5 compiler variants green
- AVR: 26,488 flash (82%), 832 RAM (41%), zero heap

## [0.1.0] - 2026-03-29

### Added
- 74 auto-generated endpoint types with [fluent builder API](docs/api-patterns.md)
- [`ApiResult<T>`](docs/working-with-responses.md) dot-access for response fields
- [Native serial and I2C transport](docs/transport.md) with CRC32, retry, and chunking
- [Type-safe duration units](docs/duration-units.md): `Seconds`, `Minutes`, `Hours`
- [`VoltageVariable`](docs/custom-field-transforms.md) builder for structured voltage thresholds
- [`JsonBuf`](docs/body-values.md) constexpr JSON builder; [`json_fmt`](docs/body-values.md) compile-time validated templates (C++20)
- [Target constraints](docs/intent-scoped-apis.md) via `make_api()` with `target<Product>()` / `target<Rat>()`
- [`body()`](docs/body-values.md) helper and `NOTE_FIELDS` macro for typed request/response bodies
- Arduino `Printable` support for response fields and errors
- [Raw JSON escape hatch](docs/raw-requests.md): `transact(json, buf)` and `send(json)`
- [Examples](examples/): getting started, sending notes, hub configuration, attention, location

### Fixed
- `set_body()` ambiguity between typed and untyped overloads
