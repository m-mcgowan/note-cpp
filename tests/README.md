# `tests/`

`note-cpp`'s test suite — host unit tests (Catch2) plus a collection of
compile-check / compile-fail fixtures and hardware integration test
projects. Everything here compiles as part of `./ci.sh`.

## Layout

- `test_*.cpp` — Catch2 unit tests. Each file targets one module or
  feature area; `CMakeLists.txt` lists them explicitly so it's clear
  what's wired in. Two harnesses share this source:
    - `note-cpp-tests` (host build, default).
    - `note-cpp-tests-arduino` (same sources under `-DARDUINO`,
      stubbed via `arduino_stubs.hpp`). Verifies `Printable`
      integration and Arduino-only code paths.
- `test_main.cpp` — `CATCH_CONFIG_MAIN`. Single TU per harness so
  Catch's CLI works.
- `catch.hpp` — vendored Catch2 single-header. Pinned locally rather
  than taken from `lib_deps` so host builds don't need the
  PlatformIO library manager.
- `arduino_stubs.hpp` — minimal Arduino API stubs (`Print`, `Stream`,
  `HardwareSerial`, etc.) used by the Arduino harness.
- `test_json_backend.hpp`, `test_notecard_factory.hpp`,
  `test_sax_exerciser.hpp` — shared test helpers.
- `migration_notec.cpp` — reference file that's compiled against the
  Blues `note-c` / `note-arduino` API, used as the source-of-truth
  for the note-c side of the migration guide's snippet markers.
- `smoke.cpp` — standalone smoke check, compiled as its own small
  binary to catch header-only link issues.
- `bench_compile_time.cpp` — build-time benchmark harness (not run by
  default; invoked on demand to measure compile-time regressions).

### Generated test files

These are rebuilt on every `./ci.sh` run from templates in
`tools/codegen/templates/` — do not edit by hand:

- `test_endpoint_coverage.cpp`
- `test_endpoint_streaming.cpp`
- `test_samples.cpp`
- `test_api_context.cpp`
- `test_sizeof_report.cpp`

### Sub-directories

- [`compile_check/`](compile_check/) — `.cpp` fixtures that must
  compile cleanly. Each is registered as a ctest that passes iff
  `-fsyntax-only` returns 0.
- [`compile_fail/`](compile_fail/) — `.cpp` fixtures that must NOT
  compile cleanly. Each is registered with `WILL_FAIL TRUE` so the
  test passes iff `-fsyntax-only` returns non-zero.
- [`compile_fail_pending/`](compile_fail_pending/) — aspirational
  compile-fail tests that can't be enforced yet (see that dir's
  README for the compiler blockers).
- [`integration/`](integration/) — out-of-tree test projects that
  build under their own CMakeLists (or PlatformIO, for hardware
  firmware). Not part of the default ctest run; each has its own
  build step in `ci.sh --full`.

## Running

```bash
./build/tests/note-cpp-tests                  # host unit tests
./build/tests/note-cpp-tests-arduino          # same, Arduino harness
ctest --test-dir build                        # everything wired via CMake
./build/tests/note-cpp-tests "[streaming]"    # filter by Catch tag
```

See [`docs/coverage.md`](../docs/coverage.md) for the coverage workflow.
