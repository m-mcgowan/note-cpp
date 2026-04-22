# `tests/compile_fail/`

Compile-fail fixtures — each `.cpp` is code that *must not* compile.
`tests/CMakeLists.txt` registers them with `WILL_FAIL TRUE`, so each
test passes iff `-fsyntax-only` returns non-zero.

The intent is to lock in error messages for typos and API misuse that
the type system is supposed to catch.

See [`../compile_fail_pending/`](../compile_fail_pending/) for
aspirational versions of the same tests that can't be enforced yet
due to compiler limitations.

## What's covered

### Intent-scoped API guards

When a `card.attn` request is in "arm" shape, it can't carry the
sleep-specific fields (and vice versa). These tests keep that
separation compile-enforced:

- `arm_has_no_disarm.cpp`, `arm_has_no_sleep.cpp`
- `off_has_no_rate.cpp`, `rearm_not_a_flag.cpp`,
  `sleep_not_a_flag.cpp`, `watchdog_not_a_flag.cpp`
- `triggers_on_sleep.cpp`, `notify_on_gps.cpp`,
  `gps_has_no_env.cpp`

### Flag-set typos

Flag fields (`.triggers`, `.notifications`, `.combo`) accept only a
known vocabulary of tokens — these verify an unknown flag value is a
compile error, not a runtime surprise:

- `invalid_trigger_flag.cpp`, `invalid_notification_flag.cpp`,
  `invalid_combo_flag.cpp`
- `trigger_assign_typo.cpp`, `notification_assign_typo.cpp`

### Enum typos

`hub.set().mode(...)` takes a validated enum string:

- `hubset_mode_typo.cpp`

### Backend constraints

Some backend modes forbid certain input shapes:

- `buffered_requires_string.cpp`, `debug_requires_string.cpp` —
  callbacks that take `string_view` can't accept `const char*` on
  certain configurations.
- `jsonb_raw_body.cpp` — raw JSON string body literals are forbidden
  when `NOTE_JSONB=1`.

### Generic API guards

- `no_api_groups.cpp` — the raw `Notecard` (without `Api<>`) does
  not expose the resource-group accessors (`nc.hub`, `nc.note`, …).

### Body validation (stubbed — see compile_fail_pending/)

- `body_array_not_object.cpp`, `body_primitive.cpp`,
  `body_trailing_comma.cpp`, `body_unquoted_key.cpp` — aim to reject
  invalid JSON body string literals at compile time. These currently
  self-fail via an `#error` stub on compilers where the consteval
  validator isn't wired up (see
  [`../compile_fail_pending/README.md`](../compile_fail_pending/README.md)
  for the plan to make these real).

## Registration

Each file is registered automatically via `file(GLOB ...)` in
`tests/CMakeLists.txt` — drop a new `.cpp` in this directory and it'll
become a ctest named `compile-fail/<filename>` on the next build.
