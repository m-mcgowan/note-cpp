# C++ version compatibility

The core library works with C++17. Each successive standard unlocks additional features without changing the API of the lower-standard subset — code that compiles on C++17 keeps working on C++20/23.

| Feature | C++17 | C++20 | C++23 |
|---------|:-----:|:-----:|:-----:|
| **Core** | | | |
| Typed API (request builders, responses, fluent setters) | yes | yes | yes |
| [Ad-hoc requests](using-the-api.md#escape-hatches) (`nc.request("hub.set", lambda)`) | yes | yes | yes |
| [Error handling](error-handling.md) | yes | yes | yes |
| [Type-safe duration units](duration-units.md) (`Seconds`, `Minutes`, `Hours`, `Days`) | yes | yes | yes |
| **JSON** | | | |
| [JSON backends](json-backend.md) (cJSON, nlohmann, `StaticJsonBackend`) | yes | yes | yes |
| [SAX streaming parser](using-the-api.md#raw-json) (`JsonSink`) | yes | yes | yes |
| [`JsonBuf` runtime builder](json-builder.md) (no allocations) | yes | yes | yes |
| [`consteval` JSON](json-builder.md) (`note::json<>()`) | — | yes | yes |
| **Body structs** | | | |
| [Body structs](body-values.md) with [`NOTE_FIELDS`](body-values.md) macro | yes | yes | yes |
| [Body structs without macro](body-values.md) (plain aggregates via reflection) | — | yes | yes |
| **Compile-time checks** | | | |
| [`consteval` enum validation](using-the-api.md#calling-styles-within-the-typed-layer) (`validatedMode()`) | — | yes | yes |
| [Target filtering](feature-flags.md#target-filtering-c20) (hardware + firmware) | — | yes | yes |
| [Version gating](feature-flags.md#api-version-gating-and-strict-mode) (per-field firmware availability) | yes | yes | yes |
| **Memory** | | | |
| [Arena sizing](arena-sizing.md) — [`MonotonicArena`](arena-sizing.md) + arena allocator | yes | yes | yes |
| [`StringPool`](memory.md) response string interning | yes | yes | yes |
| [Zero-alloc `StaticJsonBackend`](json-backend.md) (jsmn) | yes | yes | yes |
| **Standard library** | | | |
| [`std::expected`](internal/cpp-version-blockers.md) (native, vs `tl::expected` fallback) | — | — | yes |
| [`std::unreachable`](internal/cpp-version-blockers.md) (native, vs compiler builtins) | — | — | yes |
