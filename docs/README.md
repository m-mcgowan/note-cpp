# Documentation

## Getting started

1. **[Getting started example](../examples/stdcpp/getting-started.cpp)** — from setup to typed body schemas
2. **[Migration guide](platforms/arduino/migration-from-note-arduino.md)** — examples showing `note-c` and `note-cpp` side by side - quicker learning path if you're already familiar with `note-c`.

## Core features

3. **[Using the API](using-the-api.md)** — calling styles, three layers (typed → lambda → raw), focused operations, escape hatches
4. **[API reference](api-reference.md)** — complete reference for all Notecard endpoints
5. **[Working with responses](working-with-responses.md)** — field access, `has_value()`, body parsing, lifetimes
6. **[Error handling](error-handling.md)** — `Result<T>`, `ErrorInfo`, safety levels
7. **[Duration units](duration-units.md)** — `Minutes`, `Seconds`, `Hours`, `Days` with compile-time safety
8. **[Body values and Note templates](body-values.md)** — raw JSON, builder lambda, typed struct, `template_of<T>()`
9. **[JSON buffer builder](json-builder.md)** — zero-allocation `constexpr` JSON building
10. **[Environment variables](environment-variables.md)** — `env.get`/`env.set` patterns, body-into-struct parsing

## Infrastructure

11. **[Memory management](memory.md)** — zero-allocation patterns, `StringPool`, arena sizing
12. **[Feature flags](feature-flags.md)** — `NOTE_MINIMAL`, `NOTE_NO_RETRY`, AVR configuration
13. **[JSON backend](json-backend.md)** — how JSON is handled internally, available backends
14. **[Transport layer](transport.md)** — architecture, streaming vs buffered, Arduino setup
15. **[Serial transport](transport-serial.md)** — `SerialHal`, protocol constants, binary streaming
16. **[I2C transport](transport-i2c.md)** — `I2cHal`, MTU negotiation, priming query
17. **[CRC](transport-crc.md)** — auto-detection, wire format, implementation
18. **[Binary transfer](binary-transfer.md)** — `card.binary` put/get with COBS framing
19. **[JSONB wire format](jsonb.md)** — binary encoding alternative to JSON text
20. **[Response lifetimes](response-lifetimes.md)** — string_view validity, arena interning
21. **[Debugging](debugging.md)** — wire tracing, transport diagnostics
22. **[Known issues](known-issues.md)**

## Guides

23. **[Arduino guide](platforms/arduino/guide.md)** — setup, wiring, examples
24. **[ATTN pin guide](platforms/arduino/card-attn-guide.md)** — interrupt-driven wake patterns
25. **[Migrating from note-arduino](platforms/arduino/migration-from-note-arduino.md)** — side-by-side examples

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
