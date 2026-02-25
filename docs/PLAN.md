# note-cpp Project Plan

Type-safe C++23 API for the Blues Notecard. Header-only, zero dependencies beyond the standard library.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  User code                                          │
│    api.hubSet().set_product("x").execute();          │
├─────────────────────────────────────────────────────┤
│  Generated API layer        include/note/api/*.hpp  │
│    74 request/response types from OpenAPI spec       │
│    Typed fields, chainable setters, enum validation  │
├─────────────────────────────────────────────────────┤
│  Api factory                include/note/api_context.hpp │
│    Binds Notecard to generated request types         │
├─────────────────────────────────────────────────────┤
│  Notecard                   include/note/notecard.hpp│
│    Coordinates JSON backend + transport IO           │
│    execute(), command(), request() entry points      │
├──────────────────────┬──────────────────────────────┤
│  JsonBackend         │  NotecardIO                   │
│  include/note/json.hpp  │  include/note/io.hpp       │
│  Virtual JSON ops    │  Virtual transport ops        │
│  (cJSON, nlohmann,   │  (I2C, serial, queued,       │
│   RapidJSON, etc.)   │   note-c bridge, etc.)       │
└──────────────────────┴──────────────────────────────┘
```

### Key abstractions

| File | Purpose |
|------|---------|
| `types.hpp` | `Result<T>` (std::expected), `ApiResult<Response>`, `Unexpected`, version macros |
| `error.hpp` | `Error` enum, `ErrorInfo` struct |
| `json.hpp` | `JsonBackend`, `JsonBuilder`, `JsonReader` — backend-agnostic JSON interfaces |
| `json_buf.hpp` | `JsonBuf<N>` — constexpr JSON buffer builder, zero allocations |
| `io.hpp` | `NotecardIO` — transport abstraction (request/response, binary transfer) |
| `notecard.hpp` | `Notecard` — central coordinator |
| `api_context.hpp` | `Api` factory — binds Notecard to fluent request builders |
| `body.hpp` | `BodyValue`, `NOTE_BODY` macro, `template_of<T>()` — schema struct support |
| `field.hpp` | `Field<T>` — optional-like field wrapper for generated types |
| `safety.hpp` | `Safety` enum (ReadOnly, Idempotent, NonIdempotent, Destructive) |
| `api.hpp` | Umbrella header for all 74 generated endpoint types |
| `api/*.hpp` | Per-endpoint generated headers (e.g. `hub_set.hpp`, `card_version.hpp`) |

### Design principles

- **Header-only**: no .cpp files, no link step
- **Backend-agnostic**: JSON library and transport are pluggable interfaces
- **Generated from spec**: 74 endpoint types auto-generated from OpenAPI schema
- **Three body tiers**: raw JSON string, builder lambda, typed schema struct
- **Compile-time where possible**: `JsonBuf<N>` is fully constexpr, `json_const` enforces consteval
- **Dot access on results**: `ApiResult<Response>` inherits from Response for field access

## Completed Work

### Phase 1: Core abstractions
- `JsonBackend`, `JsonBuilder`, `JsonReader` interfaces
- `NotecardIO` transport interface
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
  - Tier 1: Raw JSON string (`set_body("...")`)
  - Tier 2: Builder lambda (`set_body(note::body([](auto& b) { ... }))`)
  - Tier 3: Schema struct (`set_body(readings)`)
- `NOTE_BODY` macro for C++17 struct binding
- `template_of<T>()` for Notecard template registration
- Response body parsing with `body()` and `body_as<T>()`

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

### Phase 6: OpenAPI tooling
- `tools/schema_to_openapi.py` — JSON Schema to OpenAPI 3.1 conversion
- `tools/openapi_to_schema.py` — reverse conversion
- `tools/verify_roundtrip.py` — 74/74 round-trip verification
- PR #272 on blues/notecard-schema (open)

### Infrastructure
- `ci.sh` — runs codegen, header compilation checks, unit tests, smoke test
- GitHub Actions CI (GCC 14, Clang 18)
- Examples: `getting_started.cpp`, `attention_pin.cpp`, `location_tracking.cpp`

## In Progress / Planned

### JsonBuf: auto-sized compile-time buffers
Eliminate the need to specify `JsonBuf<N>` size for compile-time-only usage.
The lambda receives the builder instead of creating it, enabling two-pass
measurement:

```cpp
// No size parameter needed — auto-measured at compile time
constexpr auto req = note::json<[](auto& b) {
    b.add("req", "hub.set");
    b.add("mode", "periodic");
    b.close();
}>();
```

Implementation: `consteval` function uses NTTP lambda, probes with large buffer
to measure, then builds with exact-sized `JsonBuf<needed>`.

### note-c string transaction API
Repo: `~/e/note-c`, branch: `feature/string-transaction-api` (on fork)

**Motivation**: `NoteRequestResponseJSON` in note-c wastefully parses
string→J*→string. A string-based API avoids this round-trip, which is
what note-cpp needs — it builds JSON strings via `JsonBuilder`/`JsonBuf`
and doesn't need cJSON internally.

**Three-layer design:**

| Layer | Function | Status |
|-------|----------|--------|
| L0 | `NoteTransactionStreaming` — callback-based, handles CRC/retry/locking on raw strings | Done |
| L1 | `NoteTransactionString` — caller-provided buffer wrapper around L0 | Done |
| L2 | Refactor `NoteRequestResponseJSON` to use L1 internally | Not started |

L0 and L1 are implemented and tested on the fork branch. L2 would make the
existing allocating API a thin wrapper around the new string API, ensuring
behavioral parity.

**Related fixes on the fork:**
- CRC memcmp bug fix (`fix/crc-memcmp-bug` branch, PR #1)
- macOS build support for unit tests (`build/macos-test-support` branch, PR #2)

### Future considerations
- **Code size metrics**: measure compiled binary size for representative examples
- **note-c bridge**: `NotecardIO` implementation that uses note-c's string transaction API as transport
- **OpenAPI Overlays**: standardized format for sideband metadata (replacing ad-hoc `safety_semantics.json` / `binary_transfer.json`)
- **C++20 reflection**: automatic struct binding without `NOTE_BODY` macro
