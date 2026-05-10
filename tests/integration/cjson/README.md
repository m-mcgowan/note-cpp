# `tests/integration/cjson/`

Host integration tests for the `CjsonBackend` — the tree-based JSON
backend used when you want typed `JsonReader` tree access on responses
(e.g. migrating from `note-c`, or when `StaticJsonBackend`'s streaming
API is a poor fit for your parse pattern).

Pulls cJSON v1.7.18 via CMake `FetchContent`; no system install needed.

## Contents

- `CMakeLists.txt` — standalone project; fetches cJSON and links.
- `test_cjson_backend.cpp` — full request/response round trips
  against the real cJSON library.
- `test_alloc_profile.cpp` — allocation counts per transaction, to
  catch regressions in the tree-build path.

## Running

```bash
cmake -B build/integration-cjson -S tests/integration/cjson
cmake --build build/integration-cjson
ctest --test-dir build/integration-cjson
```
