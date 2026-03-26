# Known Issues

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
