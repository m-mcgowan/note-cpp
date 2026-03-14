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
│  Api factory                include/note/api.hpp │
│    Binds Notecard to generated request types         │
├─────────────────────────────────────────────────────┤
│  Notecard                   include/note/notecard.hpp│
│    Coordinates JSON backend + transport callable     │
│    execute(), command(), request() entry points      │
├──────────────────────┬──────────────────────────────┤
│  JsonBackend         │  Transport callable           │
│  include/note/json.hpp│  std::function<Result<string>│
│  Virtual JSON ops    │    (string_view, uint32_t)>   │
│  (cJSON, nlohmann,   │  (provided by user/note-cpp-app)  │
│   RapidJSON, etc.)   │                               │
└──────────────────────┴──────────────────────────────┘
```

### Key abstractions

| File | Purpose |
|------|---------|
| `types.hpp` | `Result<T>` (std::expected), `ApiResult<Response>`, `Unexpected`, version macros |
| `error.hpp` | `Error` enum, `ErrorInfo` struct |
| `json.hpp` | `JsonBackend`, `JsonBuilder`, `JsonReader` — backend-agnostic JSON interfaces |
| `json_sax.hpp` | `JsonSink`, `sax_parse()` — zero-alloc streaming JSON parser with SAX callbacks |
| `json_buf.hpp` | `JsonBuf<N>` — constexpr JSON buffer builder, zero allocations |
| `allocator.hpp` | `Allocator` — function-pointer allocator with arena/pmr adapters |
| `arena.hpp` | `MonotonicArena` — bump allocator for bounded memory use |
| `notecard.hpp` | `Notecard` — central coordinator, takes `JsonBackend` + transport callable |
| `api.hpp` | `Api` factory — binds Notecard to fluent request builders |
| `body.hpp` | `BodyValue`, `NOTE_FIELDS` macro, `template_of<T>()` — schema struct support |
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
- `NOTE_FIELDS` macro for C++17 struct binding
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
- `io.hpp` deleted — transport implementations belong in note-cpp-app
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

### Phase 11: C++20 compatibility
- `tl::expected` polyfill behind `#if __cplusplus >= 202302L` guard
- Vendored `tl/expected.hpp` in `include/note/tl/`
- CI tests C++20 path with GCC 12 and Clang 17

### Phase 12: Required field enforcement
- Required fields (from upstream `notecard-schema` `required` arrays) become plain `T` members
- Factory methods take required fields as parameters — compile-time enforcement
- `api.create<T>()` bypass for incorrect annotations
- Upstream notecard-schema rebased onto v1.2.7

### Phase 13: Property extensions + schema source tracking
- `tools/property_extensions.json` sideband file for per-property `x-format` extensions
- `schema_to_openapi.py` loads and merges extensions during conversion
- `x-schema-source` embeds upstream tag + commit in OpenAPI info block

### Phase 14: Target-based RAT/firmware filtering
- `include/note/target.hpp` — `Rat`, `Product`, `Skus`, `Target<Rat, Strict>`, `Unconstrained`
- `make_api(nc)` unconstrained, `make_api(nc, target<Product>())` constrained
- Per-endpoint `static constexpr Skus skus` from `x-skus` spec metadata
- Non-strict: `[[deprecated]]` on unsupported endpoints; strict: `requires` removal
- Compile-fail CI tests for strict-mode endpoint rejection
- C++17 fallback: unconstrained only (no target support)

### Phase 15: Streaming architecture + memory control

Goal: data flows through the pipeline without unnecessary intermediate trees or
copies. The user controls where all significant memory is allocated (heap, arena,
custom allocator). The JSON wire format is an internal detail — backends are
customizable for resource tradeoffs, not because users care about JSON.

#### Completed

- **`note::Allocator`** (`include/note/allocator.hpp`) — function-pointer-based
  allocator (alloc/free/ctx). Adapters for `MonotonicArena` and
  `std::pmr::memory_resource` (when available via `__has_include`).
- **`MonotonicArena`** (`include/note/arena.hpp`) — bump allocator over a
  user-provided buffer. Reset between requests.
- **`get_builder()`** on `JsonBackend` — returns a reference to a reusable member
  builder, eliminating one `unique_ptr` allocation per request. Overridden in
  `CjsonBackend`, `CjsonArenaBackend`, `NlohmannBackend`, `BufferJsonBackend`.
- **`BufferJsonBackend`** (`include/note/backends/buffer.hpp`) — zero-heap JSON
  backend. `BufferJsonBuilder` writes directly into a fixed member buffer.
  `JsmnJsonReader` parses using vendored jsmn tokens (also member arrays). Template
  parameters control buffer and token sizes.
- **Vendored jsmn** (`include/note/backends/detail/jsmn.h`) — ~460 LOC, MIT,
  zero-alloc JSON tokenizer. Used by `JsmnJsonReader` for tree-style random access.
- **SAX JSON parser** (`include/note/json_sax.hpp`) — ~280 LOC, zero-allocation,
  single-pass streaming parser. Fires callbacks on a `JsonSink` interface. Strict
  RFC 8259 validation (100% reject rate on JSONTestSuite invalid inputs). No token
  buffer, no intermediate tree. For memory-constrained embedded targets.
- **`JsonSink`** interface — backend-agnostic callback API: `on_string`, `on_bool`,
  `on_number`, `on_null`, `on_object_begin/end`, `on_array_begin/end`. Generated
  Response types will implement thin key→field dispatch tables over this.
- **Backend documentation** (`docs/json-backend.md`) — explains that JSON is a wire
  format implementation detail, frames backend choice as resource tradeoffs.

#### Remaining

- **Response string pool** — codegen changes to `endpoint.hpp.j2` and `generate.py`.
  Generated Response types get a `JsonSink` dispatch table. String fields interned
  into a single pool allocated via `Allocator`. Primitives copied by value.
- **Transport `string_view` return** — change `RequestFn` from `Result<std::string>`
  to `Result<string_view>` into transport's member buffer. Eliminates one string copy
  per response. Response string pool copies what it needs before transport is reused.
- **`parse_into()` on `JsonBackend`** — backend-driven parse path that pushes values
  into a `JsonSink` directly. Tree backends (cJSON, nlohmann) walk their tree into
  the sink. SAX backend drives the sink from character stream. Backend choice
  determines parse strategy; generated code provides both interfaces.
- **Allocation profiling** — verify alloc counts with all backend combinations.
- **Memory guide** (`docs/memory-guide.md`) — allocator usage, backend comparison,
  arena patterns.

#### Parse strategy by backend

| Backend | Build | Parse | Tradeoff |
|---------|-------|-------|----------|
| `CjsonBackend` | cJSON tree | tree walk → sink | Debuggable, multiple small allocs |
| `CjsonArenaBackend` | cJSON tree (arena) | tree walk → sink | Debuggable, bounded memory |
| `NlohmannBackend` | nlohmann tree | tree walk → sink | Convenient if already linked |
| `BufferJsonBackend` (jsmn) | fixed buffer | token walk → sink | Zero heap, needs token array |
| SAX backend (future) | fixed buffer | streaming → sink | Zero heap, zero token buffer |

### Infrastructure
- `ci.sh` — runs codegen, header compilation checks, unit tests, smoke test
- `ci.sh --all-compilers` — discovers and tests all locally installed compilers
- `ci.sh --coverage` — lcov coverage with threshold checks (95%/95%/95%)
- GitHub Actions CI: GCC 13, GCC 14, Clang 18 (with apt package caching)
- GitHub Actions coverage: `zgosalvez/github-actions-report-lcov@v4` for PR summaries
- `tools/size_report.sh` — code size comparison (note-cpp vs note-c)
- Examples: `getting_started.cpp`, `attention_pin.cpp`, `location_tracking.cpp`,
  `sending-notes/`, `hub-configuration/`

## note-cpp-app

Higher-level app-centric library above note-cpp. Full design: `docs/note-cpp-app.md`.

### Progress

| Component          | Design  | Impl | Tests | Examples | Docs |
|--------------------|---------|------|-------|----------|------|
| DirectChannel      | done    | done | done  |    -     |  -   |
| StaticStateStore   | done    | done | done  |    -     |  -   |
| NullStateStore     | done    | done | done  |    -     |  -   |
| TemplateManager    | done    |  -   |   -   |    -     |  -   |
| SyncManager        | done    |  -   |   -   |    -     |  -   |
| AttentionManager   | done    |  -   |   -   |    -     |  -   |
| ConnectionManager  | done    |  -   |   -   |    -     |  -   |
| NotePublisher      | done    |  -   |   -   |    -     |  -   |
| ConfigManager Ph1  | done    |  -   |   -   |    -     |  -   |
| ConfigManager Ph2  | partial |  -   |   -   |    -     |  -   |
| QueuedChannel      | done    |  -   |   -   |    -     |  -   |
| TickChannel        | done    |  -   |   -   |    -     |  -   |
| Composites         | done    |  -   |   -   |    -     |  -   |
| Procedures         | done    |  -   |   -   |    -     |  -   |
| DfuManager         | outline |  -   |   -   |    -     |  -   |

### Design details

Full design: `docs/note-cpp-app.md`.

#### Foundation (implemented)
- `DirectChannel` — synchronous single-threaded wrapper (`include/note/app/channel.hpp`)
- `StaticStateStore<Types...>` — type-indexed, observable state cache (`include/note/app/state_store.hpp`)
- `NullStateStore` — no-op store

#### Managers (designed, not yet implemented)
- `TemplateManager` — session-scoped template registration cache (FNV-1a, no heap)
- `SyncManager` — hub.sync orchestration with polling, timeout, max_age
- `AttentionManager` — ATTN pin lifecycle with typed `AttnSource` bitfield
- `ConnectionManager` — hub.set/get/status lifecycle
- `NotePublisher` — transparent template registration + note.add
- `ConfigManager` — typed env var resolution, three-layer priority, validators

#### Channel variants (designed, not yet implemented)
- `QueuedChannel` — active-object pattern (deque + worker thread, RTOS)
- `TickChannel` — cooperative (one entry per `tick()`, bare-metal Arduino)
- Composites — named types for fixed request sequences
- Procedures — closures for conditional multi-step operations

#### Future
- `DfuManager` — firmware download orchestration (IDFU + ODFU)
- Hub connectivity monitor
- Note queue (batch adds, flush on sync)
- Binary transfers (`card.binary.get/put` chunking)

## Platform HAL repos

note-cpp is platform-neutral: it owns the full wire protocol (CRC, retry,
segmented TX/RX, reset sync) via injectable `SerialHal` / `I2cHal` interfaces,
matching the scope of note-c's `n_serial.c` / `n_i2c.c`. Concrete platform
glue — thin `SerialHal` / `I2cHal` subclasses — belongs in separate repos:

- `note-cpp-arduino` — Arduino `HardwareSerial` + `Wire` implementations
- `note-cpp-zephyr` — Zephyr UART + I2C driver bindings
- `note-cpp-espidf` — ESP-IDF UART + I2C bindings
- `note-cpp-linux` — `/dev/ttyACM0` serial, `/dev/i2c-N` I2C

## note-c string transaction API (superseded)

Repo: `~/e/note-c`, branch: `feature/string-transaction-api` (on fork).
L0/L1 are done; L2 not started. The note-cpp transport layer covers the same
ground at the C++ level, so L2 work on note-c is no longer a priority.

## Component status

| Feature | Design | Docs | Impl | Tests | Examples |
|---------|--------|------|------|-------|----------|
| **Core types (Result, Error, Field)** | [types.hpp](../include/note/types.hpp), [error.hpp](../include/note/error.hpp), [field.hpp](../include/note/field.hpp) | PLAN.md | same | [test_types](../tests/test_types.cpp), [test_field](../tests/test_property_functor.cpp) | |
| **JSON abstraction** | [json.hpp](../include/note/json.hpp) | PLAN.md | same | [test_json_buf](../tests/test_json_buf.cpp), [test_wire_format](../tests/test_wire_format.cpp) | |
| **Notecard coordinator** | [notecard.hpp](../include/note/notecard.hpp) | PLAN.md | same | [test_notecard](../tests/test_notecard.cpp) | |
| **74 generated API endpoints** | [api/](../include/note/api/) | PLAN.md | same | [test_endpoint_coverage](../tests/test_endpoint_coverage.cpp), [test_samples](../tests/test_samples.cpp) | |
| **Body schema (NOTE_FIELDS)** | [body.hpp](../include/note/body.hpp) | PLAN.md | same | [test_body](../tests/test_body.cpp) | [sending-notes](../examples/sending-notes/) |
| **Serial transport** | [serial.hpp](../include/note/transport/serial.hpp) | [transport.md](transport.md) | same | [test_transport_serial](../tests/test_transport_serial.cpp) | |
| **I2C transport** | [i2c.hpp](../include/note/transport/i2c.hpp) | [transport.md](transport.md) | same | [test_transport_i2c](../tests/test_transport_i2c.cpp) | |
| **constexpr JSON (JsonBuf)** | [json_buf.hpp](../include/note/json_buf.hpp) | PLAN.md | same | [test_json_buf](../tests/test_json_buf.cpp) | |
| **Protocol policy** | [protocol_policy.hpp](../include/note/transport/protocol_policy.hpp) | PLAN.md | same | [test_samples](../tests/test_samples.cpp) | |
| **Duration units** | [units.hpp](../include/note/units.hpp) | | same | [test_samples](../tests/test_samples.cpp) | |
| **VoltageVariable** | [voltage_variable.hpp](../include/note/voltage_variable.hpp) | | same | [test_voltage_variable](../tests/test_voltage_variable.cpp) | |
| **Target filtering** | [target.hpp](../include/note/target.hpp) | PLAN.md | same | [test_target](../tests/test_target.cpp), [test_make_api](../tests/test_make_api.cpp) | [target_filtering](../examples/target_filtering.cpp) |
| **Code generation tooling** | [codegen/](../tools/codegen/) | PLAN.md | [generate.py](../tools/codegen/generate.py) | | |
| **DirectChannel** | [channel.hpp](../include/note/app/channel.hpp) | [note-cpp-app.md](note-cpp-app.md) | same | [test_channel](../tests/test_channel.cpp) | |
| **StaticStateStore** | [state_store.hpp](../include/note/app/state_store.hpp) | [note-cpp-app.md](note-cpp-app.md) | same | [test_state_store](../tests/test_state_store.cpp) | |
| **Allocator** | [allocator.hpp](../include/note/allocator.hpp) | [json-backend.md](json-backend.md) | same | | |
| **MonotonicArena** | [arena.hpp](../include/note/arena.hpp) | [json-backend.md](json-backend.md) | same | [test_alloc_profile](../tests/integration/cjson/test_alloc_profile.cpp) | |
| **BufferJsonBackend** | [buffer.hpp](../include/note/backends/buffer.hpp) | [json-backend.md](json-backend.md) | same | [test_buffer_backend](../tests/integration/buffer/test_buffer_backend.cpp) | |
| **SAX parser + JsonSink** | [json_sax.hpp](../include/note/json_sax.hpp) | PLAN.md | same | [test_sax_parser](../tests/integration/buffer/test_sax_parser.cpp) | |

## Future considerations

- ~~**OpenAPI Overlays**~~: done — sideband metadata as `x-*` extensions
- ~~**C++20 compatibility**~~: done — `tl::expected` polyfill
- **C++20 reflection**: automatic struct binding without `NOTE_FIELDS` macro
