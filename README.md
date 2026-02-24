# note-cpp

Type-safe C++23 API for the [Blues Notecard](https://blues.com/notecard).

## Features

- **Generated request/response types** for all 74 Notecard API endpoints, produced from the [OpenAPI 3.1 spec](notecard-api.openapi.json)
- **Type-safe polymorphic endpoints** — overloaded Notecard requests (e.g. `note.get` query vs delete) are separate C++ types with correct safety annotations
- **`Field<T>` wrapper** — inherits from `std::optional<T>` with implicit conversion for ergonomic reads (`string_view v = req.product;`) and direct assignment (`req.product = "x";`)
- **Fluent builder API** — chainable setters: `req.set_product("x").set_mode("periodic")`
- **Designated initializer support** — request types are aggregates: `api.execute(HubSet{.mode = "periodic", .product = "x"})`
- **Instance-based `Api` factory** — bind a Notecard once, create requests with factory methods: `api.hubSet().set_product("x").execute()`
- **Polymorphic factory methods** — nested factories for dispatch variants: `api.noteGet().query()`, `api.noteGet().delete_()`
- **`req.execute(nc)` / `req.execute()`** — execute directly on any request type, with or without a bound Notecard
- **`consteval` enum validation** — compile-time checked enum values with clear error messages: `HubSet::validated_mode("typo")` fails at compile time
- **Conditional deducing-this** — setters preserve derived type through chains on compilers supporting `__cpp_explicit_this_parameter` (GCC 14+, Clang 18+)
- **Safety classification** — each request carries `ReadOnly`, `Idempotent`, `NonIdempotent`, or `Destructive` as a `constexpr`, enabling compile-time retry policy decisions
- **API version gating** — `#define NOTE_API_VERSION` controls which fields are available, with `#if` guards per field based on `x-min-api-version` from the spec
- **Binary transfer annotations** — endpoints with COBS binary data are annotated with `BinaryTransfer` metadata
- **Abstract JSON and I/O interfaces** — plug in any JSON backend (note-c/cJSON, nlohmann, etc.) and any transport (I2C, serial, queued active-object)
- **195 auto-generated wire format tests** — test expectations embedded in the OpenAPI spec as `x-validation` metadata, generated alongside the C++ types
- **Header-only** — no build system required; just add `include/` to your include path
- **C++23 with retrofit path** — uses `std::expected` with `// C++23:` comments; swap to `tl::expected` for C++17

## Quick Start

```cpp
#include <note/api_context.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_get.hpp>

// Provide your own JsonBackend and NotecardIO implementations
note::Notecard nc(backend, io);
note::Api api(nc);

// Fluent factory chain
api.hubSet().set_product("com.example.app").set_mode("periodic").execute();

// Direct field assignment
auto req = api.noteAdd();
req.file = "sensors.qo";
req.body = R"({"temp":22.5})";
req.execute();

// Designated initializers
api.execute(note::api::EnvSet{.name = "interval", .text = "300"});

// Polymorphic endpoints
api.noteGet().query().set_file("data.qi").execute();
api.noteGet().delete_().set_file("data.qi").execute();

// Compile-time validated enums
req.mode = HubSet::validated_mode("periodic");  // OK
// req.mode = HubSet::validated_mode("typo");   // compile error

// Standalone usage (without Api factory)
note::api::CardVersion ver;
auto result = ver.execute(nc);

// Ad-hoc request
auto rsp = nc.request("card.version");
```

## Code Generation

The C++ types are generated from the OpenAPI spec by a Python tool:

```bash
pip install jinja2
python3 tools/codegen/generate.py notecard-api.openapi.json
```

This generates:
- 74 per-endpoint headers in `include/note/api/`
- Umbrella header `include/note/api.hpp`
- Api factory `include/note/api_context.hpp`
- 195 wire format tests in `tests/test_samples.cpp`

## CI

```bash
./ci.sh
```

Runs code generation, header compilation checks (gcc + clang), unit tests (221 test cases), and a smoke test.
