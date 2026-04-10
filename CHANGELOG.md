# Changelog

<!--
All notable changes to this project will be documented in this file.
Follows [Keep a Changelog](https://keepachangelog.com/) conventions.

Write entries from the library user's perspective. Internal implementation
details belong in git commit messages and design docs, not here.
-->

## [Unreleased]

### Added
- `has_value()` on response fields — detect whether a field was present in the JSON response
- `.into(T&)` on request builders — stream response body fields directly into a user struct
- `Response::max_arena_size` and `RequestSet<Req...>::max_arena_size` — compile-time arena budgets
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
- 74 auto-generated endpoint types from OpenAPI 3.1 spec with fluent builder API
- `ApiResult<T>` dot-access for response fields (`rsp.temperature`, `rsp.connected`)
- Native serial and I2C transport with CRC32, retry, and chunking
- Type-safe duration units: `Seconds`, `Minutes`, `Hours`
- `VoltageVariable` builder for structured voltage thresholds
- `JsonBuf` constexpr JSON builder; `json_fmt` compile-time validated templates (C++20)
- Target constraints via `make_api()` with `target<Product>()` / `target<Rat>()`
- `body()` helper and `NOTE_FIELDS` macro for typed request/response bodies
- Arduino `Printable` support for response fields and errors
- Raw JSON escape hatch: `transact(json, buf)` and `send(json)`
- [Examples](examples/): getting started, sending notes, hub configuration, attention, location

### Fixed
- `set_body()` ambiguity between typed and untyped overloads
