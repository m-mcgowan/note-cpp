# Documentation

This index groups docs by what you're trying to do. **New here?** Start with [Getting started](getting-started.md) — a top-down walkthrough from a clean project to your first request. After that, the rest of the tree splits into how-to guides (task-oriented), reference (lookup), architecture and concepts (understanding the library's shape), and platforms (Arduino-specific).

## Start here

- **[Getting started](getting-started.md)** — top-down walkthrough from a clean project to your first request
- **[Glossary](glossary.md)** — Notecard wire vocabulary vs `note-cpp` library vocabulary, side by side
- **[FAQ](faq.md)** — short answers to the questions readers hit before they know where to look
- **[Runnable example](../examples/stdcpp/getting-started.cpp)** — code companion for the Getting started walkthrough

## Migrating

- **[From note-arduino](platforms/arduino/migration-from-note-arduino.md)** — side-by-side examples for Arduino sketches
- **[From note-c (host)](platforms/host/migration-from-note-c.md)** — bridge-transport adoption for incremental host-side migration

## How-to guides

Task-oriented; read them when you need to do the thing they describe.

- **[Environment variables](environment-variables.md)** — `env.get`/`env.set` patterns, body-into-struct parsing
- **[Binary transfers](binary-transfer.md)** — `card.binary` put/get with COBS framing
- **[Debugging wire traffic](debugging.md)** — wire tracing, transport diagnostics
- **[Troubleshooting](troubleshooting.md)** — symptom → cause → fix catalog for common user-side footguns
- **[Production deployment](production-deployment.md)** — flags, OTA, watchdog, log routing, reset recovery for shipped firmware

## Reference

Lookup material — keep tab-handy, scan as needed.

- **[API reference](api-reference.md)** — every Notecard endpoint, every field, generated from the spec
- **[Feature flags](feature-flags.md)** — `NOTE_MINIMAL`, `NOTE_NO_RETRY`, full compile-time switch catalog
- **[C++ version compatibility](cpp-version-compatibility.md)** — feature × C++17/20/23 matrix
- **[Duration units](duration-units.md)** — `Minutes`, `Seconds`, `Hours`, `Days` with compile-time safety
- **[JSON buffer builder](json-builder.md)** — `JsonBuf`, `note::json<>()`, the constexpr builder
- **[Serial transport](transport-serial.md)** — `SerialHal`, protocol constants, binary streaming
- **[I2C transport](transport-i2c.md)** — `I2cHal`, MTU negotiation, priming query
- **[CRC](transport-crc.md)** — auto-detection, wire format, implementation
- **[JSONB wire format](jsonb.md)** — binary encoding alternative to JSON text
- **[Known issues](known-issues.md)** — confirmed library bugs with workarounds
- **[Quality assurance](quality-assurance.md)** — test matrix, coverage numbers, doc verification

## Architecture and concepts

Understanding-oriented — read when you want to know *why* the library is shaped the way it is.

- **[Using the API](using-the-api.md)** — calling styles, three layers (typed → lambda → raw), focused operations, escape hatches
- **[Composition](composition.md)** — wire format × response presentation × binary payload as three independent axes, with the matrix of combinations
- **[Streaming and tree modes](streaming-and-tree.md)** — the two JSON-parsing strategies, what they share, where they diverge
- **[JSON backends](json-backend.md)** — `CjsonBackend`, `StaticJsonBackend`, `NlohmannBackend`, when to pick which
- **[Working with responses](working-with-responses.md)** — field access, `has_value()`, body parsing, lifetimes
- **[Error handling](error-handling.md)** — `Result<T>`, `ErrorInfo`, safety levels
- **[Body values and Note templates](body-values.md)** — raw JSON, builder lambda, typed struct, `template_of<T>()`
- **[Memory management](memory.md)** — zero-allocation patterns, `StringPool`, arena sizing
- **[Response lifetimes](response-lifetimes.md)** — `string_view` validity, arena interning

## Platforms

- **[Arduino guide](platforms/arduino/guide.md)** — setup, wiring, examples, binary-size patterns
- **[ATTN pin guide](platforms/arduino/card-attn-guide.md)** — interrupt-driven wake patterns

## Contributing

Internal documentation for contributors working on `note-cpp` itself:

- **[API design](internal/api-design.md)** — two-layer architecture, naming conventions
- **[Code generation](internal/codegen.md)** — OpenAPI spec → C++ headers pipeline
- **[Coverage](internal/coverage.md)** — GCC + lcov methodology
- **[Documentation generation](doxygen/documentation.md)** — Doxygen site
- **[Retry design](internal/retry-design.md)** — transport retry and safety levels
- **[Streaming transport](internal/streaming-transport.md)** — SAX pipeline internals
- **[Strict body-field validation](internal/strict-body-fields.md)** — `NOTE_STRICT_BODY_FIELDS` mechanism, design and validation paths
- **[Custom field transforms](internal/custom-field-transforms.md)** — design notes for body-struct conversion across the note-cpp / embedded-config-cpp / note-cpp-app family
- **[Release checklist](internal/release-checklist.md)**

## Examples

| Example | Description |
|---------|-------------|
| [getting-started.cpp](../examples/stdcpp/getting-started.cpp) | Four tiers: ad-hoc, constexpr JSON, typed API, body schemas |
| [hub-configuration/](../examples/stdcpp/hub-configuration/) | Units, named constants, consteval validation, voltage-variable sync |
| [sending-notes/](../examples/stdcpp/sending-notes/) | All body patterns: raw, lambda, struct, template, receive, command |
| [target-filtering.cpp](../examples/stdcpp/target-filtering.cpp) | Hardware and firmware targeting with compile-time warnings/errors |
| [zero-alloc.cpp](../examples/stdcpp/zero-alloc.cpp) | Zero-allocation patterns: StaticJsonBackend, StringPool, CjsonArena |
| [env-vars.cpp](../examples/stdcpp/env-vars.cpp) | All four env.get modes: single, multi-into-struct, all, change-polling |
| [streaming-and-tree.cpp](../examples/stdcpp/streaming-and-tree.cpp) | Streaming vs tree response presentation with `body()` / `.into()` patterns |
| [wire-format-and-presentation.cpp](../examples/stdcpp/wire-format-and-presentation.cpp) | The wire × presentation matrix end to end — same demo() runs in all four cells |
