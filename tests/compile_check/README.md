# `tests/compile_check/`

Compile-only fixtures that verify the library compiles in configurations
where runtime testing is awkward (different feature flags, reduced
API surface, etc.). Each `.cpp` is registered as a ctest that invokes
`-fsyntax-only` and passes iff compilation succeeds.

Complement to [`compile_fail/`](../compile_fail/) — same mechanism,
opposite expectation.

## Contents

Each file is self-contained and compiled exactly once:

- `api_groups.cpp` — the generated compile-check (via
  `tools/codegen/templates/compile_check_api_groups.cpp.j2`). Every
  endpoint must be reachable through the `Api<>` resource-group
  accessors (`nc.hub.set()`, `nc.note.add()`, etc.). Regenerated on
  every `./ci.sh`.
- `buffered_disabled.cpp` — compiles with `-DNOTE_NO_BUFFERED=1`;
  confirms that the streaming-only path still builds without any of
  the buffered-transport types in scope.
- `extras_disabled.cpp` — compiles with `-DNOTE_NO_EXTRAS=1`,
  verifying that optional extension hooks can be stripped cleanly.
- `field_types.cpp` — instantiates every generated field type to
  catch `ResponseField` template instantiation errors that might
  otherwise only appear per-consumer.
- `jsonb_body_alternatives.cpp` — under `NOTE_JSONB=1`, raw string
  bodies are a compile error (intentional). This file compiles the
  *supported* body-setter shapes (`body(...)` lambdas, typed
  structs) to ensure they still work.
- `minimal_mode.cpp` — `-DNOTE_MINIMAL=1` full build-check. Most AVR
  targets use this flag combination; this guards against a compile-
  time regression.
- `printable_disabled.cpp` — `-DNOTE_NO_PRINTABLE=1`; verifies the
  Arduino `Printable` integration is optional.
- `request_set_sizing.cpp` — exercises `RequestSet<...>::max_arena_size`
  instantiations for every generated endpoint type.
- `singleton_mode.cpp` — `-DNOTE_SINGLETON=1`; checks the
  singleton-Api variant compiles. See
  [`project_singleton_ci.md`](../../docs/internal/) (referenced from
  codegen docs) for the motivation.
- `units_via_api_hpp.cpp` — confirms that including `<note/api.hpp>`
  alone is enough to use `60_mins`, `2_hours`, etc. (users shouldn't
  have to also pull in `<note/units.hpp>`).

## Registration

Each file is registered automatically via `file(GLOB ...)` in
`tests/CMakeLists.txt` — drop a new `.cpp` in this directory and it'll
become a ctest named `compile-check/<filename>` on the next build.
