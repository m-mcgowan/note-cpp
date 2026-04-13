# Documentation

## Getting started

1. **[Getting started example](../examples/getting_started.cpp)** — from setup to typed body schemas
2. **[Migration guide](guides/migration-from-note-arduino.md)** — side-by-side comparison with `note-c`, pattern-by-pattern


## Core features

3. **[API calling patterns](api-patterns.md)** — fluent, assignment, conditional, fire-and-forget
4. **[Working with responses](working-with-responses.md)** — field access, `has_value()`, body parsing, lifetimes
5. **[Error handling](error-handling.md)** — `Result<T>`, `ErrorInfo`, safety levels
6. **[Intent-scoped APIs](intent-scoped-apis.md)** — distinct types for multi-purpose endpoints
7. **[Duration units](duration-units.md)** — `Minutes`, `Seconds`, `Hours`, `Days` with compile-time safety
8. **[Body values and Note templates](body-values.md)** — raw JSON, builder lambda, typed struct, `template_of<T>()`
9. **[Custom field transforms](custom-field-transforms.md)** — `VoltageVariable`, comma-separated flags
10. **[JSON buffer builder](json-builder.md)** — zero-allocation `constexpr` JSON building
11. **[Raw requests](raw-requests.md)** — escape hatch for requests not covered by the typed API

## Infrastructure

12. **[Memory management](memory.md)** — zero-allocation patterns, `StringPool`, arena sizing
13. **[Feature flags](feature-flags.md)** — `NOTE_MINIMAL`, `NOTE_NO_RETRY`, AVR configuration
14. **[JSON backend](json-backend.md)** — how JSON is handled internally, available backends
15. **[Transport layer](transport.md)** — serial and I2C protocols, HAL interfaces, CRC
16. **[Binary transfer](binary-transfer.md)** — `card.binary` put/get with COBS framing
17. **[JSONB wire format](jsonb.md)** — binary encoding alternative to JSON text
18. **[Streaming vs buffered](streaming-vs-buffered.md)** — when to use each path, migration from note-c
17. **[Response lifetimes](response-lifetimes.md)** — string_view validity, arena interning
18. **[C++ standard requirements](cpp-standard-requirements.md)** — what each standard version enables
19. **[Debugging](debugging.md)** — wire tracing, transport diagnostics
20. **[Known issues](known-issues.md)**

## Guides

21. **[Arduino guide](platforms/arduino/guide.md)** — setup, wiring, examples
22. **[ATTN pin guide](guides/card-attn-guide.md)** — interrupt-driven wake patterns
23. **[Migrating from note-arduino](guides/migration-from-note-arduino.md)** — side-by-side examples

## Contributing

Internal documentation for contributors:

- **[API design](internal/api-design.md)** — two-layer architecture, naming conventions
- **[Code generation](internal/codegen.md)** — OpenAPI spec → C++ headers pipeline
- **[Coverage](internal/coverage.md)** — GCC + lcov methodology
- **[Documentation generation](doxygen/documentation.md)** — Doxygen site
- **[Retry design](internal/retry-design.md)** — transport retry and safety levels
- **[Streaming transport](internal/streaming-transport.md)** — SAX pipeline internals
- **[Release checklist](internal/release-checklist.md)**

## Examples

| Example | Description |
|---------|-------------|
| [getting_started.cpp](../examples/getting_started.cpp) | Four tiers: ad-hoc, constexpr JSON, typed API, body schemas |
| [hub-configuration/](../examples/hub-configuration/) | Units, named constants, consteval validation, voltage-variable sync |
| [sending-notes/](../examples/sending-notes/) | All body patterns: raw, lambda, struct, template, receive, command |
| [target_filtering.cpp](../examples/target_filtering.cpp) | Hardware and firmware targeting with compile-time warnings/errors |
| [zero_alloc.cpp](../examples/zero_alloc.cpp) | Zero-allocation patterns: BufferJsonBackend, StringPool, CjsonArena |
