# `tests/compile_fail_pending/` — aspirational compile-fail tests

This directory mirrors `tests/compile_fail/` by filename but **is not
wired into CMakeLists.txt**. Nothing compiles these files during a
normal build or CI run. They exist as a *specification* of compile-fail
behavior the library should eventually enforce.

The four tests in here share names with real active tests under
`tests/compile_fail/`:

| File | What the ideal version should reject at compile time |
|------|--------------------------------------------------------|
| `body_array_not_object.cpp` | `req.body = "[1,2,3]"` — top-level array where an object is required |
| `body_primitive.cpp`        | `req.body = "42"` or `"\"str\""` — non-object primitive |
| `body_trailing_comma.cpp`   | `req.body = R"({"t":1,})"` — malformed JSON |
| `body_unquoted_key.cpp`     | `req.body = R"({t:1})"` — malformed JSON |

The `compile_fail/` siblings of these files guard themselves with
`#error` on compilers where the validation can't run today, so they
trivially pass by failing compilation on the stub line. The copies in
*this* directory use looser guards and would try to exercise the real
validator on every GCC, which can't happen yet — see below.

## What's blocking the move to `compile_fail/`

`include/note/body.hpp` gates the consteval `BodyValue(const char (&)[N])`
constructor on `C++20 && !clang && __GNUC__ >= 14`:

```cpp
#elif __cplusplus >= 202002L && !defined(__clang__) \
    && !(defined(__GNUC__) && __GNUC__ < 14)
    template<std::size_t N>
    consteval BodyValue(const char (&s)[N]) { ... json_valid(...) ... }
```

Two separate compiler blockers live inside that guard:

1. **Apple Clang (all versions as of Xcode 15): `consteval` constructor
   materialization with `std::optional` is broken.** The validator runs
   at compile time and even reports errors correctly, but the resulting
   `optional` comes out as `has_value() == false` — the validated value
   is silently dropped. Rather than let a typo compile and ship empty,
   Clang is excluded outright (`#if !defined(__clang__)`) and the field
   falls back to the unvalidated `Field::operator=`. Documented in
   `docs/known-issues.md` under *"Apple Clang: no compile-time
   validation of string literal assignment"*.

2. **GCC < 14: inherited-consteval interaction.** The `consteval`
   constructor template above, combined with `BodyValue`'s other
   `constexpr`/templated constructors, trips a pre-GCC-14 regression
   (tracked upstream as PR 102933). The validation either doesn't
   trigger or rejects well-formed input. So GCC 13 and earlier fall
   through to the non-consteval `BodyValue(string_view)` path, which
   doesn't validate.

The `compile_fail/` stubs use `#if __clang__ || __GNUC__ < 14` so they
neutralize themselves on every compiler where validation isn't live.
The `compile_fail_pending/` copies use `#if __clang__` only — they
*would* expect the real validator on GCC 13+. That's the test we can't
run yet.

## Plan for a future session: unblock and move these to `compile_fail/`

**Before doing the implementation work below, brainstorm the exact pattern
to use with the user — there are multiple reasonable shapes (factory
helper, deduction guide, single-template free function, …) and the
trade-offs touch downstream code.**

### 1. Research & scope (est. 30 min)

- Reproduce the GCC 13 failure mode directly. Write a minimal consteval
  ctor that matches `BodyValue`'s template shape and confirm the exact
  symptom on GCC 13.2 — is it PR 102933 as the comment claims, or
  something else? The comment in `body.hpp` attributes it to "inherited
  consteval constructors" but `BodyValue` has no base class. Verify.
- Check the Apple Clang status: last-known-bad Xcode version, any
  public Clang trunk fix. (See `docs/known-issues.md` for prior
  evidence.) If Xcode 16/17 ships a fix, the `!defined(__clang__)`
  guard can just go away.

### 2. Pick a direction

Three realistic options, from least to most invasive:

- **Option A — leave it alone, delete `compile_fail_pending/`.**
  If both blockers are permanent for the supported compiler matrix, the
  pending tests have no runway. The `compile_fail/` stubs already cover
  what's enforceable today. Cost: lose the aspirational TODO artifact,
  document intent in a regular markdown instead.

- **Option B — rework `BodyValue`'s consteval ctor to sidestep the GCC
  13 regression.** Candidate patterns to try:
    1. Replace the templated consteval ctor with a free function
       (`consteval BodyValue make_body(const char(&)[N])`) and a
       deduction guide / implicit-conversion chain.
    2. Use a non-template consteval ctor that takes `std::string_view`
       directly, with the conversion from `const char[N]` happening at
       the call site via a helper.
    3. Drop the `template<std::size_t N>` in favour of
       `std::string_view` + `static_assert` at the caller. Less DX but
       bypasses the templated-ctor path entirely.
  If any of these compiles+validates on GCC 13, drop the
  `__GNUC__ < 14` half of the guard and move the pending tests in.

- **Option C — raise the minimum supported GCC to 14.** Cleanest code,
  real cost: Debian stable ships GCC 13 today, Ubuntu 24.04 ships
  GCC 13.2. Would require a CHANGELOG note and a README matrix update.
  Only consider this if GCC 14 crosses the mainstream threshold (check
  distro defaults at planning time).

### 3. Apply the chosen fix

- If **Option B** works: update `include/note/body.hpp`, regenerate
  nothing (header-only), run the full compile-fail matrix under GCC 13
  and GCC 14. Then `git mv tests/compile_fail_pending/*.cpp
  tests/compile_fail/` (overwriting the stubs), adjust the `#if` guard
  inside each to match the now-current support level, and delete this
  README along with the empty directory.
- If **Option C**: update `library.json`, `library.properties`,
  CMakeLists minimum, CI matrix, README compiler-support row. Then same
  move as Option B.
- If **Option A**: delete this directory, keep the `compile_fail/`
  stubs as-is (they document intent via their `#error` message), and
  add a one-line TODO to `docs/known-issues.md` pointing at the
  consteval-body-validation section so the blocker stays discoverable.

### 4. Acceptance

- `ctest -R compile-fail/body_` passes on every supported compiler.
- Running each moved test manually with the #error removed shows the
  real JSON validator firing (grep the compiler output for
  `invalid JSON or not a top-level object`).
- `docs/known-issues.md` updated to reflect whichever blockers still
  stand.
