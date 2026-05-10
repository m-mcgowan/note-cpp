# `tests/integration/buffer/`

Host integration tests for the `StaticJsonBackend` — the zero-alloc
jsmn-based JSON backend used on AVR and other constrained targets.
Separate from the main Catch2 suite because the main suite uses stubbed
backends; here we exercise the real parser/builder end to end.

## Contents

- `CMakeLists.txt` — standalone CMake project (depends only on
  `note-cpp` headers, no external backend needed).
- `test_buffer_backend.cpp` — buffer-backend request/response round
  trips covering every field type.
- `test_sax_parser.cpp` — direct jsmn-lexer tests (iteration, error
  positions, recovery).
- `test_sax_alloc_profile.cpp` / `test_alloc_profile.cpp` —
  allocation profiling harness that confirms the backend stays
  zero-alloc in the expected paths.

## Running

```bash
cmake -B build/integration-buffer -S tests/integration/buffer
cmake --build build/integration-buffer
ctest --test-dir build/integration-buffer
```
