# `cmake/`

CMake include files that expose `note-cpp`'s source and test lists to
both the in-repo `tests/CMakeLists.txt` and downstream `add_subdirectory`
consumers.

## Contents

- `note-cpp-sources.cmake` — hand-curated lists: the public headers
  (`NOTE_CPP_PUBLIC_HEADERS`), Arduino-only headers, transport
  headers, backend headers, vendored third-party headers, and the
  manually-written test sources (`NOTE_CPP_TEST_SOURCES_COMMON` +
  `NOTE_CPP_TEST_SOURCES_FULL_ONLY`). Edit this file when you add a
  new hand-written header or test.
- `note-cpp-generated.cmake` — **generated.** Rebuilt on every
  `./ci.sh` run by `tools/codegen/generate.py`. Lists every generated
  header (`include/note/api.hpp`, `include/note/api/*.hpp`) and the
  generated test TUs. Paths are repo-relative, so the checked-in file
  is machine-independent.

## Usage

Downstream projects just need `add_subdirectory(note-cpp)` —
`CMakeLists.txt` at the repo root pulls both files in the right order.
If you're building a custom tree that needs the lists without the
targets, include them directly:

```cmake
include(note-cpp/cmake/note-cpp-sources.cmake)
include(note-cpp/cmake/note-cpp-generated.cmake)
# Now NOTE_CPP_PUBLIC_HEADERS and NOTE_CPP_GENERATED_HEADERS are set.
```
