# C++ version compatibility

The core library works with C++17. Each successive standard unlocks additional features without changing the API of the lower-standard subset — code that compiles on C++17 keeps working on C++20/23.

// TODO - link to the typed API
| Group/Feature | C++17 | C++20 | C++23 |
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

// TODO - what is the significance of the standard library features - how do these differences affect user code other than the concrete types have different names but the same API service? How is std::unreachable used? Are these internal library details?

## Setting the standard in your build

The library requires C++17 or later. C++20 unlocks designated initializers, duck-typed args structs, and `consteval` enum validation; the rest of the surface is the same.

// TODO - isn't build_unflags also needed when the defaults also include setting the -std value? look at our actual platformio.ini examples.

### PlatformIO (Arduino framework)

```ini
; platformio.ini
[env:myboard]
build_flags = -std=gnu++20    ; or gnu++23
```

Common platform defaults:

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

### Zephyr

In `prj.conf` or your board's config:

```
CONFIG_STD_CPP20=y
```

Or in `CMakeLists.txt`:

```cmake
set(CMAKE_CXX_STANDARD 20)
```

// TODO - are there tests for zephyr builds? If not, we shouldn't mention it in these docs.