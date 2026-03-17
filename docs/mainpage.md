\mainpage note-cpp API Reference

`note-cpp` is a modern C++ SDK for the [Blues Notecard](https://blues.com).
It provides a fully typed, compile-time-safe API generated from the Notecard
API specification.

## Key types

- note::Notecard — transport abstraction and request execution engine
- note::Api — typed accessors for all Notecard API endpoints (e.g. `api.hub.set()`)
- note::ApiResult — result type returned by request execution (value or note::ErrorInfo)
- note::JsonBuilder / note::JsonReader — JSON serialization interfaces
- note::JsonBuf — built-in zero-dependency JSON backend
- note::StringPool — arena-backed string interning for response lifetimes
- note::Allocator — pluggable memory allocator
- note::BodyValue — type-safe `body` field accessor

## Duration and unit types

- note::Seconds, note::Minutes, note::Hours, note::Days — prevent accidental mixing of time units
- note::VoltageVariable — type-safe builder for adaptive sync intervals (USB/high/normal/low/dead)
- note::Firmware — firmware version representation for version gating

## Filtering and validation

- note::FlagSet — comma-separated bitfield with named methods and compile-time constants (e.g. `note::attn::arm`)
- note::Target / note::Skus — compile-time SKU checks (ensures requests match the connected Notecard hardware)
- `consteval` validators — catch invalid mode/enum strings at compile time

## Request/response types

- `note::api` — namespace containing generated types for every Notecard API endpoint
- Each endpoint struct provides: a fluent request builder, `execute()` / `command()` methods, and a typed `Response` with parsed fields
- Fields gated by firmware version emit deprecation warnings when targeting older firmware

## Getting started

See the [README](https://github.com/m-mcgowan/note-cpp) and `examples/` directory
for usage patterns.
