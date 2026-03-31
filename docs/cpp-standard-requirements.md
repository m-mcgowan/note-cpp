# C++ Standard Requirements

note-cpp currently targets C++17. This document catalogs every C++ standard
feature used and whether it could be lowered to C++14 with polyfills.

## Summary

| Feature | Standard | Usage count | C++14 polyfill | Effort |
|---------|----------|-------------|----------------|--------|
| `if constexpr` | C++17 | ~15 sites | Tag dispatch / SFINAE overloads | Medium |
| `std::optional` | C++17 | 10 headers | `tl::optional` (already vendor `tl::expected`) | Drop-in |
| `std::string_view` | C++17 | 20 headers | Already aliased as `note::string_view` | Drop-in¹ |
| `std::void_t` | C++17 | 4 headers | `template<class...> using void_t = void;` | Trivial |
| `_v` variable templates | C++17 | ~20 sites | `::value` instead of `_v` | Mechanical |
| `inline` variables | C++17 | ~15 constants | `constexpr` (works in headers, different linkage) | Trivial |
| Nested namespaces `a::b` | C++17 | 2 headers | `namespace a { namespace b {` | Trivial |
| `std::pmr` | C++17 | 1 header | Already `#if __has_include` guarded | None |
| Fold expressions | C++17 | 2 sites | Only in C++20 reflection path (guarded) | None |
| Structured bindings | C++17 | 0 | Not used | N/A |
| `[[nodiscard]]` | C++17 | 0 in core | Not used | N/A |
| `[[maybe_unused]]` | C++17 | 0 in core | Not used | N/A |

¹ `std::string_view` is already aliased through `note::string_view` in `types.hpp`.
A polyfill would replace the stdlib include with a custom implementation on
platforms that lack it (e.g. AVR).

## Detailed Analysis

### `if constexpr` (C++17) — Medium effort to lower

Used in `body.hpp` for type-dispatching field serialization:
```cpp
if constexpr (std::is_same_v<V, bool>) {
    b.add(name, val);
} else if constexpr (std::is_integral_v<V>) {
    b.add(name, static_cast<int32_t>(val));
} ...
```

Also used in `notecard.hpp` for binary pipeline detection:
```cpp
if constexpr (detail::has_binary_src<RequestT>::value) {
    if (req.has_binary_data()) return do_binary_send(req);
}
```

**C++14 alternative:** Tag dispatch or SFINAE overload sets. More verbose
but equivalent. The body.hpp dispatching could use a visitor pattern or
overloaded function templates instead.

### `std::optional` (C++17) — Drop-in polyfill

`Field<T>` inherits from `std::optional<T>`:
```cpp
struct RequestField : std::optional<T> { ... };
```

`Notecard` stores `std::optional<Allocator>` and `ApiResult` stores
`std::optional<ErrorInfo>`.

**C++14 alternative:** `tl::optional` — same author as `tl::expected`
which we already vendor. Single header, CC0 license, C++11 compatible.

### `std::void_t` (C++17) — Trivial polyfill

Used for SFINAE detection traits:
```cpp
template<typename T>
struct has_binary_src<T, std::void_t<decltype(...)>> : std::true_type {};
```

**C++14 alternative:**
```cpp
template<class...> using void_t = void;  // 1 line
```

### `_v` variable templates (C++17) — Mechanical replacement

`std::is_same_v<T,U>` → `std::is_same<T,U>::value`
`std::is_convertible_v<T,U>` → `std::is_convertible<T,U>::value`
`std::decay_t<T>` already works in C++14.

~20 sites, purely mechanical find-and-replace (or a `_v` polyfill template).

### `inline` variables (C++17) — Low risk

Transport constants (`kI2cDefaultMtu`, `cobs_eop`, etc.) use `inline constexpr`.

**C++14 alternative:** `constexpr` without `inline`. In a header-only
library this risks ODR violations if the constant is ODR-used (address
taken). In practice, embedded code doesn't take addresses of these
constants, so `static constexpr` or `constexpr` in a namespace works.

### Nested namespaces `namespace a::b` (C++17) — Trivial

Only in `arduino.hpp`: `namespace note::arduino {`.

**C++14 alternative:** `namespace note { namespace arduino {`.

### `std::pmr` (C++17) — Already guarded

`allocator.hpp` wraps `std::pmr::memory_resource` behind
`#if __has_include(<memory_resource>)`. No change needed.

### Fold expressions (C++17)

Only in the C++20 reflection path (`body.hpp`), which is already
`#if __cplusplus >= 202002L` guarded. Not used on C++17.

## What blocks C++14 today

1. **`if constexpr`** — 15 sites, needs tag dispatch redesign
2. **`std::optional`** — vendor `tl::optional` (trivial)
3. **`std::void_t`** — 1-line polyfill
4. **`_v` templates** — mechanical replacement
5. **`inline` variables** — change to `static constexpr`
6. **Nested namespaces** — change to nested `namespace` blocks

Items 2-6 are trivial. Item 1 is the only real work — redesigning the
type-dispatch in `body.hpp` and the binary detection in `notecard.hpp`
to use SFINAE overloads instead of `if constexpr`.

## `std::string` elimination progress

| Header | Before | After | Status |
|--------|--------|-------|--------|
| `md5.hpp` | `std::string` return | `Md5Hex` (33-byte fixed buffer) | ✅ Done |
| `notecard.hpp` | `std::string` local | `Md5Hex` | ✅ Done |
| `error.hpp` | `std::string` return | `ErrorString` (256-byte fixed buffer) | ✅ Done |
| `json.hpp` | `to_string()` + `view_buf_` | `to_view()` pure virtual, no string member | ✅ Done |
| `transport.hpp` | `wire_`, `response_buf_` | Streaming transport bypasses; legacy path remains | ⬜ Blocked on `crc_add()` |
| `crc32.hpp` | `crc_add()` takes/returns `std::string` | Needs char-buffer variant | ⬜ TODO |

The streaming transport (`transact_streaming`, `set_receive_buffer`, `transact_into`)
bypasses both `wire_` and `response_buf_` entirely. The `std::string` members are
only used by the legacy `transact()` fallback path.

For platforms without `<string>` (AVR), the streaming path + `BufferJsonBackend`
avoids the fallback — but the compiler still parses the `std::string` member
declarations. Full AVR support requires either:
- A `std::string` polyfill from the compat project (fixed-capacity, no heap)
- Or replacing `wire_`/`response_buf_` with a fixed-size buffer type

## What blocks AVR specifically

Beyond C++ standard features, AVR's stock toolchain (avr-g++ 5.0) lacks
the entire C++ standard library — even `<cstddef>` is missing. The C
headers (`<stddef.h>`, `<stdint.h>`, `<string.h>`) work fine.

AVR needs:
1. All the C++14 changes above
2. Polyfills for: `string_view`, `optional`, `type_traits`, `utility`
3. `std::string` polyfill or replacement in `transport.hpp` + `crc32.hpp`
4. Elimination of `std::function` (use virtual interfaces or function pointers)
5. Elimination of `std::variant` (use tagged union)
6. C headers instead of C++ wrappers (`<stddef.h>` not `<cstddef>`)

See the AVR compatibility plan for the incremental approach.

## Platform Support Matrix

| Platform | Compiler | C++ stdlib | Min standard | Status |
|----------|----------|-----------|-------------|--------|
| ESP32 (pioarduino) | xtensa-g++ 14.2 | Full | C++17 | ✓ Works |
| STM32 (STM32duino) | arm-g++ 12+ | Full | C++17 | ✓ Works |
| RP2040 (Pico) | arm-g++ 12+ | Full | C++17 | ✓ Works |
| nRF52/53 (Zephyr) | arm-g++ 12+ | Full | C++17 | Expected |
| AVR (Arduino Uno) | avr-g++ 5.0 | None | C++17 lang | Needs polyfills |
