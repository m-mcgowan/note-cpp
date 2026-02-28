# note-cpp Project Plan

Type-safe C++23 API for the Blues Notecard. Header-only, zero dependencies beyond the standard library.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  User code                                          │
│    api.hubSet().product("x").execute();              │
├─────────────────────────────────────────────────────┤
│  Generated API layer        include/note/api/*.hpp  │
│    74 request/response types from OpenAPI spec       │
│    Typed fields, chainable setters, enum validation  │
├─────────────────────────────────────────────────────┤
│  Api factory                include/note/api_context.hpp │
│    Binds Notecard to generated request types         │
├─────────────────────────────────────────────────────┤
│  Notecard                   include/note/notecard.hpp│
│    Coordinates JSON backend + transport callable     │
│    execute(), command(), request() entry points      │
├──────────────────────┬──────────────────────────────┤
│  JsonBackend         │  Transport callable           │
│  include/note/json.hpp│  std::function<Result<string>│
│  Virtual JSON ops    │    (string_view, uint32_t)>   │
│  (cJSON, nlohmann,   │  (provided by user/note-app)  │
│   RapidJSON, etc.)   │                               │
└──────────────────────┴──────────────────────────────┘
```

### Key abstractions

| File | Purpose |
|------|---------|
| `types.hpp` | `Result<T>` (std::expected), `ApiResult<Response>`, `Unexpected`, version macros |
| `error.hpp` | `Error` enum, `ErrorInfo` struct |
| `json.hpp` | `JsonBackend`, `JsonBuilder`, `JsonReader` — backend-agnostic JSON interfaces |
| `json_buf.hpp` | `JsonBuf<N>` — constexpr JSON buffer builder, zero allocations |
| `notecard.hpp` | `Notecard` — central coordinator, takes `JsonBackend` + transport callable |
| `api_context.hpp` | `Api` factory — binds Notecard to fluent request builders |
| `body.hpp` | `BodyValue`, `NOTE_BODY` macro, `template_of<T>()` — schema struct support |
| `field.hpp` | `Field<T>` — optional-like field wrapper for generated types |
| `safety.hpp` | `Safety` enum (ReadOnly, Idempotent, NonIdempotent, Destructive) |
| `api.hpp` | Umbrella header for all 74 generated endpoint types |
| `api/*.hpp` | Per-endpoint generated headers (e.g. `hub_set.hpp`, `card_version.hpp`) |

### Design principles

- **Header-only**: no .cpp files, no link step
- **Transport-agnostic**: JSON library is pluggable, transport is a simple callable (string in, string out)
- **Generated from spec**: 74 endpoint types auto-generated from OpenAPI schema
- **Three body tiers**: raw JSON string, builder lambda, typed schema struct
- **Compile-time where possible**: `JsonBuf<N>` is fully constexpr, `json_const` enforces consteval
- **Dot access on results**: `ApiResult<Response>` inherits from Response for field access

## Completed Work

### Phase 1: Core abstractions
- `JsonBackend`, `JsonBuilder`, `JsonReader` interfaces
- `Notecard` coordinator with `execute()`, `command()`, `request()`
- `Result<T>` / `Unexpected` error handling
- `Safety` enum and per-request safety classification

### Phase 2: Code generation
- OpenAPI spec (`notecard-api.openapi.json`) with 74 endpoints
- `tools/codegen/generate.py` — Jinja2-based code generator
- 74 per-endpoint headers in `include/note/api/`
- Umbrella header, Api factory
- Typed fields, chainable setters, compile-time enum validation
- API version gating via `NOTE_API_VERSION`
- 195 wire-format sample tests in `tests/test_samples.cpp`

### Phase 3: Body values and schemas
- `BodyValue` type supporting three tiers:
  - Tier 1: Raw JSON string (`body("...")`)
  - Tier 2: Builder lambda (`body(note::body([](auto& b) { ... }))`)
  - Tier 3: Schema struct (`body(readings)`)
- `NOTE_BODY` macro for C++17 struct binding
- `template_of<T>()` for Notecard template registration
- Response body parsing with `body()` and `bodyAs<T>()`

### Phase 4: ApiResult
- `ApiResult<Response>` — inherits from Response for dot-access
- `r.version` instead of `r->version`
- All examples updated to dot notation

### Phase 5: JsonBuf
- `JsonBuf<N>` constexpr JSON buffer builder
- Composable fragments: `JsonBuf::object()`, `JsonBuf::array()`
- `json_const()` consteval wrapper — compile error if not compile-time resolvable
- Overflow diagnostics: snprintf-style `size()` reporting
- 14 compile-time static_assert tests + 11 runtime tests

### Phase 6: Auto-sized JsonBuf
- `note::json<lambda>()` — auto-sized consteval builder, no buffer size needed
- `json_const()` overflow check — compile error if buffer too small

### Phase 7: Transport simplification
- Removed `NotecardIO` virtual interface and `json_handle` opaque type
- Transport is now a simple callable: `(string_view request, uint32_t timeout) → Result<string>`
- `JsonBuilder::release()` replaced with `JsonBuilder::to_string()`
- `JsonBackend::wrap_response()/free_response()` replaced with `parse_response(string_view)`
- `io.hpp` deleted — transport implementations belong in note-app
- Tests simplified: `CapturingIO` replaced with inline lambdas

### Phase 8: OpenAPI tooling
- `tools/schema_to_openapi.py` — JSON Schema to OpenAPI 3.1 conversion
- `tools/openapi_to_schema.py` — reverse conversion
- `tools/verify_roundtrip.py` — 74/74 round-trip verification
- PR #272 on blues/notecard-schema (open)

### Phase 9: Code size metrics
- `tools/size_report.sh` — reproducible binary size and call-site comparison
- Benchmarks: minimal, 5-api, all-74-api, body-tiers scenarios
- Side-by-side comparison with note-c (same operations, mock transport)
- Per-function call-site analysis isolating caller overhead
- Results (-Os, Apple Clang 15):
  - Binary: note-cpp 25-28 KB vs note-c 67 KB (-58% to -62%)
  - Call-site: ~50 bytes/function average overhead for type safety
  - `card_version` caller is smaller in note-cpp (typed response vs manual JGetString)

### Phase 10: Qt-style property accessors
- Per-field functor types (container_of pattern) — each field is callable, returns parent for chaining
- Direct field assignment and designated initializers preserved (aggregate stays public)
- `extra(key, value)` for undocumented properties; `operator[]` as syntactic sugar
- `DynField` proxy type with type-erased setter; `ExtraSlot` fixed-size buffer (default 4)
- camelCase accessor names: `note_id` → `noteId`, `body_as<T>()` → `bodyAs<T>()`
- `validatedMode()` etc. renamed from `validated_mode()`
- `dyn_field.hpp` new header; `endpoint.hpp.j2` template fully rewritten
- 33 new tests in `test_property_functor.cpp`

### Infrastructure
- `ci.sh` — runs codegen, header compilation checks, unit tests, smoke test
- `ci.sh --all-compilers` — discovers and tests all locally installed compilers
- GitHub Actions CI: GCC 13, GCC 14, Clang 18 (with apt package caching)
- `tools/size_report.sh` — code size comparison (note-cpp vs note-c)
- Examples: `getting_started.cpp`, `attention_pin.cpp`, `location_tracking.cpp`

## Planned

### note-app

Higher-level app-centric library above note-cpp. Full design: `docs/note-app.md`.

Key abstractions:
- `INoteChannel` — the interface all app components depend on (not note-cpp directly)
- `DirectChannel`, `QueuedChannel`, `TickChannel` — implementations
- Composites — named types for fixed, non-conditional request sequences
- Procedures — closures for conditional multi-step operations (no rollback)
- Re-entrancy handling — composites/procedures run to completion in one tick
- `AttentionManager`, `ConfigManager<T>`, `SyncManager` — planned components

### Platform HAL repos
note-cpp is platform-neutral: it owns the full wire protocol (CRC, retry,
segmented TX/RX, reset sync) via injectable `SerialHal` / `I2cHal` interfaces,
matching the scope of note-c's `n_serial.c` / `n_i2c.c`. Concrete platform
glue — thin `SerialHal` / `I2cHal` subclasses — belongs in separate repos:

- `note-arduino-cpp` — Arduino `HardwareSerial` + `Wire` implementations
- `note-zephyr-cpp` — Zephyr UART + I2C driver bindings
- `note-espidf-cpp` — ESP-IDF UART + I2C bindings
- `note-linux-cpp` — `/dev/ttyACM0` serial, `/dev/i2c-N` I2C

Each of these depends on note-cpp and provides only the HAL glue.
Application-level patterns (periodic sync, DFU orchestration) can live in
whichever repo is most appropriate.

### note-c string transaction API (superseded)
Repo: `~/e/note-c`, branch: `feature/string-transaction-api` (on fork).
L0/L1 are done; L2 not started. The note-cpp transport layer covers the same
ground at the C++ level, so L2 work on note-c is no longer a priority.

### Future considerations
- **OpenAPI Overlays**: standardized format for sideband metadata
- **C++20 reflection**: automatic struct binding without `NOTE_BODY` macro
- **C++20 compatibility**: current minimum is C++23 due to `std::expected`. Dropping to C++20
  requires swapping `std::expected`/`std::unexpected` in `types.hpp` for `tl::expected` (or a
  vendored polyfill) behind a `#if __cplusplus >= 202302L` guard. Concepts and `consteval` are
  already C++20. C++17 would additionally require removing concepts and replacing `consteval`
  with `constexpr`, losing some compile-time safety. C++20 is the realistic target for embedded
  toolchains (Arduino/ESP32/STM32 ship GCC 10–12).
