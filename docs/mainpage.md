\mainpage note-cpp API Reference

`note-cpp` is a modern C++ SDK for the [Blues Notecard](https://blues.com).
It provides a fully typed, compile-time-safe API generated from the Notecard
OpenAPI specification.

## Key namespaces

- `note::api` — Generated request/response types for every Notecard API endpoint
- `note` — Core types: Notecard, JsonBuilder, JsonReader, Result, units, etc.

## Highlights

- **Type-safe request builders** — fluent chaining or direct field assignment
- **Duration units** — `note::Minutes`, `note::Seconds` prevent accidental mixing
- **Voltage-variable fields** — type-safe builders for adaptive sync intervals
- **Flag fields** — `FlagSet` with named methods and compile-time constants
- **Compile-time validation** — `consteval` validators catch typos in mode strings
- **Version gating** — fields gated by firmware version with deprecation warnings
- **Target filtering** — compile-time radio access technology (RAT) checks

## Getting started

See the [README](https://github.com/blues/note-cpp) and `examples/` directory
for usage patterns.
