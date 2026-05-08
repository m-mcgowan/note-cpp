# Documentation

This index is the full doc tree, ordered by audience. **If you're new, start with [Getting started](getting-started.md)** — it walks you from a clean project to your first request. **Core features** covers the typed API and the language-level tools built on top; **Infrastructure** is for transport, JSON, memory, and the topics you reach for once your app is talking to the Notecard. **Guides** covers platform-specific setup and migration paths.

## Getting started

1. **[Getting started](getting-started.md)** — top-down walkthrough from a clean project to your first request
2. **[Glossary](glossary.md)** — Notecard wire vocabulary vs `note-cpp` library vocabulary, side by side
3. **[FAQ](faq.md)** — short answers to the questions readers hit before they know where to look
4. **[Getting started example](../examples/stdcpp/getting-started.cpp)** — runnable code companion for the walkthrough
5. **[Migration guide](platforms/arduino/migration-from-note-arduino.md)** — examples showing `note-c` and `note-cpp` side by side - quicker learning path if you're already familiar with `note-c`.

## Core features

6. **[Using the API](using-the-api.md)** — calling styles, three layers (typed → lambda → raw), focused operations, escape hatches
7. **[API reference](api-reference.md)** — complete reference for all Notecard endpoints
8. **[Working with responses](working-with-responses.md)** — field access, `has_value()`, body parsing, lifetimes
9. **[Error handling](error-handling.md)** — `Result<T>`, `ErrorInfo`, safety levels
10. **[Duration units](duration-units.md)** — `Minutes`, `Seconds`, `Hours`, `Days` with compile-time safety
11. **[Body values and Note templates](body-values.md)** — raw JSON, builder lambda, typed struct, `template_of<T>()`
12. **[JSON buffer builder](json-builder.md)** — zero-allocation `constexpr` JSON building
13. **[Environment variables](environment-variables.md)** — `env.get`/`env.set` patterns, body-into-struct parsing

## Infrastructure

14. **[Memory management](memory.md)** — zero-allocation patterns, `StringPool`, arena sizing
15. **[Feature flags](feature-flags.md)** — `NOTE_MINIMAL`, `NOTE_NO_RETRY`, AVR configuration
16. **[JSON backend](json-backend.md)** — how JSON is handled internally, available backends
17. **[Transport layer](transport.md)** — architecture, streaming vs buffered, Arduino setup
18. **[Serial transport](transport-serial.md)** — `SerialHal`, protocol constants, binary streaming
19. **[I2C transport](transport-i2c.md)** — `I2cHal`, MTU negotiation, priming query
20. **[CRC](transport-crc.md)** — auto-detection, wire format, implementation
21. **[Binary transfer](binary-transfer.md)** — `card.binary` put/get with COBS framing
22. **[JSONB wire format](jsonb.md)** — binary encoding alternative to JSON text
23. **[Response lifetimes](response-lifetimes.md)** — string_view validity, arena interning
24. **[Debugging](debugging.md)** — wire tracing, transport diagnostics
25. **[Known issues](known-issues.md)** — confirmed library bugs with workarounds
26. **[Troubleshooting](troubleshooting.md)** — symptom → cause → fix catalog for common user-side footguns
27. **[Production deployment](production-deployment.md)** — flags, OTA, watchdog, log routing, reset recovery for shipped firmware

## Guides

28. **[Arduino guide](platforms/arduino/guide.md)** — setup, wiring, examples
29. **[ATTN pin guide](platforms/arduino/card-attn-guide.md)** — interrupt-driven wake patterns
30. **[Migrating from note-arduino](platforms/arduino/migration-from-note-arduino.md)** — side-by-side examples
31. **[Migrating from note-c (host)](platforms/host/migration-from-note-c.md)** — host-side migration with bridge transport for incremental adoption

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
