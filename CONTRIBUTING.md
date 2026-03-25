# Contributing to note-cpp

Bug reports, feature requests, and pull requests are welcome.

By submitting a contribution, you agree that your work is licensed
under the project's [MIT License](LICENSE).

## Development setup

note-cpp is header-only — no compilation step for the library itself.
You need a C++17 compiler (C++20 recommended) and Python 3 for code generation.

## Building and testing

```bash
./ci.sh              # quick: codegen + unit tests (~15s)
./ci.sh --full       # release: headers, examples, version gating, docs, coverage
./ci.sh --coverage   # coverage report (requires GCC 13+ and lcov 2.x)
```

CMake (parallel compilation via Ninja):

```bash
cmake -G Ninja -B build -S .
cmake --build build --parallel
ctest --test-dir build
```

The CMake build produces two test binaries:
- `note-cpp-tests` — host environment (no ARDUINO)
- `note-cpp-tests-arduino` — simulated Arduino environment (ARDUINO=10812)

Arduino-cli (ESP32):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc examples/arduino/serial_basic
```

## Code generation

API types are generated from the [Notecard OpenAPI spec](notecard-api.openapi.json):

```bash
pip install jinja2
python3 tools/codegen/generate.py notecard-api.openapi.json
```

This generates:
- 74 endpoint headers in `src/note/api/`
- `src/note/api.hpp` (the Api class)
- 3 test files in `tests/`
- `cmake/note-cpp-generated.cmake` (file listing for CMake)

Generated files are committed to the repo so users don't need Python.
Run codegen after modifying templates in `tools/codegen/templates/` or
updating the OpenAPI spec.

## Project structure

```
src/note/          Headers (canonical location; include/ is a symlink)
src/note/api/      Generated endpoint headers (74 files)
src/note.hpp       Arduino gateway header
tests/             Unit tests (CMake discovers via cmake/note-cpp-sources.cmake)
tools/codegen/     Code generator (Python + Jinja2)
examples/          Arduino sketches, PlatformIO examples, binary size comparison
docs/              Design documents, migration guide
cmake/             CMake source lists (curated + generated)
```

## CI

The default `./ci.sh` runs codegen and unit tests (~15 seconds).
`./ci.sh --full` adds header compilation, examples, version gating,
target filtering, doc verification, and coverage. Every run logs to
`ci.log` with per-stage timing.
