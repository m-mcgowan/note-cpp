# `tests/integration/nlohmann/`

Host integration tests for the `NlohmannBackend` — the C++-idiomatic
JSON backend for projects that already use
[nlohmann/json](https://github.com/nlohmann/json). Pulls json v3.11.3
via CMake `FetchContent`; no system install needed.

## Contents

- `CMakeLists.txt` — standalone project; fetches nlohmann/json and
  links.
- `test_nlohmann_backend.cpp` — request/response round trips and
  interoperability with `nlohmann::json` values (round-trip through
  `.dump()`, assignment to `json` tree, body-struct serialization
  via ADL).

## Running

```bash
cmake -B build/integration-nlohmann -S tests/integration/nlohmann
cmake --build build/integration-nlohmann
ctest --test-dir build/integration-nlohmann
```
