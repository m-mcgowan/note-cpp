# Documentation

## Getting started

1. **[Getting started example](../examples/stdcpp/getting-started.cpp)** — from setup to typed body schemas
2. **[Migration guide](platforms/arduino/migration-from-note-arduino.md)** — examples showing `note-c` and `note-cpp` side by side - quicker learning path if you're already familiar with `note-c`.

## Core features

3. **[API layers](api-layers.md)** — typed API → base requests → lambda builder → raw JSON
4. **[API calling patterns](api-patterns.md)** — fluent, assignment, conditional, fire-and-forget
5. **[API reference](api-reference.md)** — complete reference for all Notecard endpoints
6. **[Working with responses](working-with-responses.md)** — field access, `has_value()`, body parsing, lifetimes
7. **[Error handling](error-handling.md)** — `Result<T>`, `ErrorInfo`, safety levels
8. **[Intent-focused APIs](intent-focused-apis.md)** — named intents for multi-purpose endpoints
9. **[Intent-scoped APIs](intent-scoped-apis.md)** — polymorphic dispatch types and safety levels
10. **[Duration units](duration-units.md)** — `Minutes`, `Seconds`, `Hours`, `Days` with compile-time safety
11. **[Body values and Note templates](body-values.md)** — raw JSON, builder lambda, typed struct, `template_of<T>()`
12. **[Custom field transforms](custom-field-transforms.md)** — `VoltageVariable`, comma-separated flags
13. **[JSON buffer builder](json-builder.md)** — zero-allocation `constexpr` JSON building
14. **[Environment variables](environment-variables.md)** — `env.get`/`env.set` patterns, body-into-struct parsing
15. **[Raw requests](raw-requests.md)** — escape hatch for requests not covered by the typed API

## Infrastructure

15. **[Memory management](memory.md)** — zero-allocation patterns, `StringPool`, arena sizing
16. **[Feature flags](feature-flags.md)** — `NOTE_MINIMAL`, `NOTE_NO_RETRY`, AVR configuration
17. **[JSON backend](json-backend.md)** — how JSON is handled internally, available backends
18. **[Transport layer](transport.md)** — architecture, streaming vs buffered, Arduino setup
19. **[Serial transport](transport-serial.md)** — `SerialHal`, protocol constants, binary streaming
20. **[I2C transport](transport-i2c.md)** — `I2CHal`, MTU negotiation, priming query
21. **[CRC](transport-crc.md)** — auto-detection, wire format, implementation
22. **[Binary transfer](binary-transfer.md)** — `card.binary` put/get with COBS framing
23. **[JSONB wire format](jsonb.md)** — binary encoding alternative to JSON text
22. **[Response lifetimes](response-lifetimes.md)** — string_view validity, arena interning
23. **[C++ standard requirements](cpp-standard-requirements.md)** — what each standard version enables
24. **[Debugging](debugging.md)** — wire tracing, transport diagnostics
25. **[Known issues](known-issues.md)**

## Guides

26. **[Arduino guide](platforms/arduino/guide.md)** — setup, wiring, examples
27. **[ATTN pin guide](platforms/arduino/card-attn-guide.md)** — interrupt-driven wake patterns
28. **[Migrating from note-arduino](platforms/arduino/migration-from-note-arduino.md)** — side-by-side examples

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
| [getting-started.cpp](../examples/stdcpp/getting-started.cpp) | Four tiers: ad-hoc, constexpr JSON, typed API, body schemas |
| [hub-configuration/](../examples/stdcpp/hub-configuration/) | Units, named constants, consteval validation, voltage-variable sync |
| [sending-notes/](../examples/stdcpp/sending-notes/) | All body patterns: raw, lambda, struct, template, receive, command |
| [target-filtering.cpp](../examples/stdcpp/target-filtering.cpp) | Hardware and firmware targeting with compile-time warnings/errors |
| [zero-alloc.cpp](../examples/stdcpp/zero-alloc.cpp) | Zero-allocation patterns: BufferJsonBackend, StringPool, CjsonArena |
| [env-vars.cpp](../examples/stdcpp/env-vars.cpp) | All four env.get modes: single, multi-into-struct, all, change-polling |
