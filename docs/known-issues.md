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

## JSONB + NOTE_MINIMAL: raw JSON string bodies remain a compile error

**Affects:** builds with `NOTE_JSONB=1` *and* `NOTE_MINIMAL=1` (the
AVR / 32 KB-flash profile).

**Symptom:** `req.body = R"({"temp":22.5})"` fails with "no viable
overloaded `=`" on the AVR build.

**Cause:** The general `add_raw()` implementation under JSONB
SAX-parses the fragment and replays the events as opcodes. That pulls
~6 KB of full-text JSON parser, `parse_double`, and `snprintf("%g", …)`
into the binary — more than the ATmega328P's 32 KB flash budget can
absorb. Under `NOTE_MINIMAL` the impl is therefore a no-op, and the
`BodyValue` raw-string constructor is correspondingly disabled to
prevent silent data loss.

**Workaround:** Use a builder lambda or typed struct — both shapes
never go through `add_raw` and so don't pull the lexer in:

```cpp
req.body(note::body([](note::JsonBuilder& b) { b.add("temp", 22.5); }));
req.body(Readings{.temperature = 22.5, .humidity = 60});
```

**Resolved at runtime under `NOTE_JSONB && !NOTE_MINIMAL`:** non-AVR
JSONB builds support `req.body = R"(...)"` directly — the same surface
as under JSON. See `tests/compile_check/jsonb_raw_body.cpp` and the
`add_raw` cases in `tests/test_jsonb.cpp`.
