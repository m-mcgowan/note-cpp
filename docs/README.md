# note-cpp Documentation

## Getting started

1. **[Why note-cpp?](comparison.md)** — side-by-side comparison with `note-c` showing what type safety buys you
2. **[Getting started example](../examples/getting_started.cpp)** — four tiers of the API, from raw JSON to typed body schemas

## Core features

3. **[Error handling](error-handling.md)** — `Result<T>`, `ErrorInfo` with phase-based `Error` + diagnostic `Cause`, safety levels and retry guidance
4. **[Duration units](duration-units.md)** — `Minutes`, `Seconds`, `Hours`, `Days` with compile-time unit safety, voltage-variable sync
5. **[Body values and Note templates](body-values.md)** — three tiers (raw JSON, builder lambda, typed struct), template registration with `template_of<T>()`
6. **[Polymorphic APIs](polymorphic-apis.md)** — endpoints with multiple behaviors modeled as distinct types (`NoteGet::Get` vs `NoteGet::Delete`)
7. **[JSON buffer builder](json-builder.md)** — zero-allocation `constexpr` JSON building with `JsonBuf`

## Infrastructure

8. **[Transport layer](transport.md)** — serial and I2C protocol implementations, HAL interfaces, CRC, segmented TX/RX
9. **[Coverage](coverage.md)** — test coverage methodology, GCC + lcov 2.x requirements

## App layer

10. **[App design](note-cpp-app.md)** — higher-level app abstractions: channels, state stores, managers
11. **[App orchestration](app-orchestration.md)** — NTN/satellite handling, template lifecycle, sync direction management, composed setup procedures

## Project

12. **[Project plan](PLAN.md)** — architecture, completed phases, component status, roadmap

## Examples

| Example | Description |
|---------|-------------|
| [getting_started.cpp](../examples/getting_started.cpp) | Four tiers: ad-hoc, constexpr JSON, typed API, body schemas |
| [hub-configuration/](../examples/hub-configuration/) | Units, named constants, consteval validation, voltage-variable sync |
| [sending-notes/](../examples/sending-notes/) | All body patterns: raw, lambda, struct, template, receive, command |
| [target_filtering.cpp](../examples/target_filtering.cpp) | Product targeting with compile-time warnings/errors |
