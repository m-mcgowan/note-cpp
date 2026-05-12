# Known issues

## Apple Clang: no compile-time validation of string literal assignment

**Affects:** Apple Clang (all versions as of Xcode 15). Does not affect GCC 13+.

**Symptom:** Assigning an invalid string literal to an enum or flag field
compiles without error:

```cpp
req.mode = "perioidc";        // should fail, but compiles on Clang
req.notifications = "typo";   // should fail, but compiles on Clang
```

On GCC C++20, both produce compile errors via `consteval` validation.

**Cause:** Apple Clang has a bug in `consteval` constructor materialization
with `std::optional`. A `consteval` constructor correctly validates at
compile time, but the result has `has_value() == false` at runtime — the
optional doesn't engage. This makes the validated value silently empty.

To avoid silent data loss, consteval constructors are disabled on Clang
(`#if !defined(__clang__)`). Clang falls back to the unvalidated
`using Field::operator=` path.

**Workaround:** Use named constants or fluent methods instead of string
literals — these are always type-safe on all compilers:

```cpp
req.mode = mode_t::periodic;          // always safe
req.notifications.env().dfu();         // always safe
req.triggers = attn::connected;        // always safe
```

**Tracking:** This will be re-evaluated when Apple ships a newer Clang
with the fix. The `#if !defined(__clang__)` guards can be removed once
the bug is resolved. The compile-fail tests have `#error` skips for
Clang so they'll start failing (correctly) when the fix lands.

## JSONB: raw JSON string bodies are a compile error

**Affects:** builds with `NOTE_JSONB=1` (including `NOTE_MINIMAL`).

**Symptom:** `req.body = R"({"temp":22.5})"` or `req.body("json string")`
fails to compile with "no viable overloaded '='".

**Cause:** JSONB cannot embed raw JSON text fragments. The `add_raw()`
builder method is a no-op in JSONB mode, so the raw-string `BodyValue`
constructors are disabled to prevent silent data loss.

**Workaround:** Use a builder lambda or typed struct instead:

```cpp
// Lambda body — works with both JSON and JSONB
req.body(note::body([](note::JsonBuilder& b) {
    b.add("temp", 22.5);
}));

// Typed struct body — works with both JSON and JSONB
req.body(Readings{.temperature = 22.5, .humidity = 60});
```

A compile-fail test (`tests/compile_fail/jsonb_raw_body.cpp`) verifies
this behavior.
