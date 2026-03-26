# json_fmt — Compile-time validated JSON templates with runtime values

## Status: Proposed

## Motivation

`JsonBuf` is correct by construction but requires a builder pattern.
Raw JSON strings are concise but only validated on GCC C++20. A
printf-like format combines the best of both — concise inline JSON
with compile-time structural validation and runtime value insertion.

## API

```cpp
// Any type — {} accepts int, float, string, bool
nc.note.add()
    .file("sensors.qo")
    .body(note::json_fmt(R"({"temp":{},"name":{}})", temp, name))
    .execute();

// Typed placeholders — compile error if argument type doesn't match
nc.note.add()
    .file("sensors.qo")
    .body(note::json_fmt(R"({"temp":{f},"count":{i},"active":{b},"label":{s}})",
                         temp, count, active, label))
    .execute();
```

## Placeholder types

| Placeholder | Accepts | JSON output |
|---|---|---|
| `{}` | any serializable type | type-appropriate |
| `{i}` | `int32_t`, `int16_t`, `int8_t` | unquoted integer |
| `{f}` | `float`, `double` | unquoted number |
| `{b}` | `bool` | `true` / `false` |
| `{s}` | `string_view`, `const char*` | quoted string |

## Compile-time checks

1. **JSON structure** — the template (with `{}` placeholders treated as
   valid values) is parsed by the consteval JSON validator. Malformed
   JSON is a compile error.
2. **Placeholder count** — number of `{}` / `{x}` must match number of
   arguments. Mismatch is a compile error.
3. **Type matching** — `{i}` requires an integer argument, `{s}` requires
   a string, etc. Wrong type is a compile error. `{}` accepts anything.
4. **Top-level object** — the template must be a JSON object (not array
   or primitive).

## Runtime behavior

`json_fmt` returns a `BodyValue` that, when `write_to()` is called,
walks the template string and substitutes placeholders with the
captured argument values using `JsonBuilder::add_raw` for the
pre-validated structural parts and `JsonBuilder::add` for the values.

Alternatively, it could write directly into a `JsonBuf<N>` where N
is computed from the template size + estimated value sizes.

## Implementation sketch

```cpp
template<detail::FixedString Fmt, typename... Args>
consteval auto json_fmt_check() {
    // 1. Count placeholders in Fmt
    // 2. Verify count == sizeof...(Args)
    // 3. If typed placeholders, verify type compatibility
    // 4. Replace {} with dummy values, validate as JSON object
    // 5. Return a tag type encoding the format + types
}

template<detail::FixedString Fmt, typename... Args>
auto json_fmt(Args&&... args) {
    constexpr auto check = json_fmt_check<Fmt, Args...>();
    return BodyValue(/* captured args + format template */);
}
```

The `FixedString` NTTP (C++20) captures the format string at compile
time. The `consteval` check function validates everything. The runtime
function captures the arguments and returns a `BodyValue`.

## Interaction with existing body approaches

`json_fmt` sits between JsonBuf and raw strings in the safety ranking:

1. Typed struct (fully type-safe)
2. Compile-time JsonBuf (structure + values validated)
3. **json_fmt** (structure validated, values type-checked)
4. Runtime JsonBuf (structure correct by construction)
5. Builder lambda (structure correct by construction)
6. Raw JSON string (validated on GCC C++20, unvalidated on Clang)

## Open questions

- Should `json_fmt` require `close()` / auto-close like JsonBuf?
  Probably not — the template string has the closing `}`.
- Should it support nested placeholders (`{"location":{}}` where the
  placeholder is itself a JSON object)? Probably not in v1.
- Should conditional fields be supported (`{"temp":{},"gps":{?}}`
  where `{?}` is omitted if the argument is nullopt)? Interesting
  but complex.
