\mainpage note-cpp API Reference

`note-cpp` is a modern C++ SDK for the [Blues Notecard](https://blues.com).
It provides a fully typed, compile-time-safe API generated from the Notecard
API specification.

## Key types

- note::Notecard — request execution engine (JSON build → transport → parse)
- note::Api — typed accessors for all Notecard API endpoints (e.g. `api.hub.set()`)
- note::ApiResult — result type returned by request execution (value or note::ErrorInfo)
- note::JsonBuilder / note::JsonReader — JSON serialization interfaces
- note::JsonBuf — built-in zero-dependency JSON backend
- note::StringPool — arena-backed string interning for response lifetimes
- note::Allocator — pluggable memory allocator
- note::BodyValue — type-safe `body` field accessor

## Transport

- note::ITransact — pure virtual transport interface (transact, send, reset, abort)
- note::AbstractTransport — base class with shared retry/CRC logic; subclasses implement raw byte I/O
- note::transport::NotecardSerial — serial wire protocol implementation
- note::transport::NotecardI2c — I2C wire protocol implementation
- note::test::CallbackTransport — adapter for wrapping lambdas as ITransact (testing)

## Duration and unit types

- note::Seconds, note::Minutes, note::Hours, note::Days — prevent accidental mixing of time units
- note::VoltageVariable — type-safe builder for adaptive sync intervals (USB/high/normal/low/dead)
- note::Firmware — firmware version representation for version gating

## Filtering and validation

- note::FlagSet — comma-separated bitfield with named methods and compile-time constants (e.g. `note::attn::connected`)
- note::Target / note::Skus / note::Product — compile-time product checks (ensures requests match the connected Notecard SKU)
- `consteval` validators — catch invalid enum strings and flag combinations at compile time (C++20)

## Request/response types

- `note::api` — namespace containing generated types for every Notecard API endpoint
- Each endpoint struct provides: a fluent request builder, `execute()` / `command()` methods, and a typed `Response` with parsed fields
- Fields gated by firmware version emit deprecation warnings when targeting older firmware

## Examples

@example getting_started.cpp Walks through four levels of abstraction: ad-hoc JSON requests, compile-time JSON, the fully typed API with autocomplete, and body schemas for structured data.
@example hub-configuration/main.cpp Type-safe duration units, named constants, and voltage-variable sync intervals.
@example sending-notes/main.cpp Sending and receiving data: note.add, note.get, templates, body schemas.
@example zero_alloc.cpp Zero-allocation patterns for memory-constrained embedded systems.
@example target_filtering.cpp Compile-time product checks — ensure requests match the connected Notecard SKU.
@example attention_pin.cpp ATTN pin interrupt handling with card.attn.
@example location_tracking.cpp GPS location tracking with polymorphic card.location APIs.

## Getting started

See the [README](https://github.com/m-mcgowan/note-cpp) and the examples above
for usage patterns.
