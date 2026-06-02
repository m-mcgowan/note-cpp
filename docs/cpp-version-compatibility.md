# C++ version compatibility

The core library works with C++17. Each successive standard unlocks additional features without changing the API of the lower-standard subset — code that compiles on C++17 keeps working on C++20/23.

| Group/Feature | C++17 | C++20 | C++23 |
|---------|:-----:|:-----:|:-----:|
| **Core** | | | |
| [Typed API](using-the-api.md) (request builders, responses, fluent setters) | yes | yes | yes |
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
The library's internal use of C++23 standard-library types (`std::expected`,
`std::unreachable`) is hidden behind `note::expected` / `ApiResult` and a
`NOTE_UNREACHABLE()` macro respectively. User-facing types are stable
across C++17/20/23; the C++23 path swaps in standard-library equivalents
for `tl::expected` and compiler builtins with no surface change. See
[internal/cpp-version-blockers.md](internal/cpp-version-blockers.md) if
you're curious about the substitutions.

## Setting the standard in your build

The library requires C++17 or later. C++20 unlocks designated initializers, duck-typed args structs, and `consteval` enum validation; the rest of the surface is the same.

### PlatformIO (Arduino framework)

Most PlatformIO Arduino platforms ship with a default `-std=gnu++11` (or
`gnu++14`) baked into their toolchain wrapper, so simply adding
`-std=gnu++20` to `build_flags` *appends* a second `-std` flag rather than
replacing the old one — and gcc honours the last `-std` on the command
line, which on some platforms is still the default. Use `build_unflags`
to drop the platform's default and then set the version you want:

```ini
; platformio.ini
[env:myboard]
build_unflags = -std=gnu++11    ; remove the platform default
build_flags   = -std=gnu++20    ; or gnu++23
```

The note-cpp binary-size-comparison `platformio.ini` follows this pattern
on every AVR/ESP/ARM env.

Common platform defaults that need unflagging:

- **ESP32 (pioarduino)**: defaults to `gnu++11`. Set `-std=gnu++23` for full C++20 features.
- **nRF52 / nRF53 (Arduino)**: defaults to `gnu++11`. Set `-std=gnu++17` or higher.
- **STM32 (STM32duino)**: defaults to `gnu++14`. Set `-std=gnu++17` or higher.

### PlatformIO (ESP-IDF framework)

```ini
; platformio.ini — ESP-IDF uses CMake, not build_flags for C++ standard
build_flags = -std=gnu++20
```

Or in your component's `CMakeLists.txt`:

```cmake
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
```

