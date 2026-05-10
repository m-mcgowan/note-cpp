# `tests/integration/`

Out-of-tree test projects that sit outside the main Catch2 host suite.
Each sub-directory builds on its own (separate `CMakeLists.txt` or
`platformio.ini`) so it can pull in dependencies the host suite doesn't
use — real JSON backends, ESP32 hardware, a Notecard simulator, etc.

None of these run in the default `ctest`; `./ci.sh --full` drives them,
and `validate-release.sh` enforces them as release-gate steps.

## Sub-directories

- [`firmware/`](firmware/) — PlatformIO project that uploads a
  doctest runner to an ESP32-S3 (MPCB 1.9 / 1.10) and drives it
  against a real Notecard over serial or I2C. The on-hardware
  regression suite.
- [`buffer/`](buffer/) — host tests for the `StaticJsonBackend`
  (jsmn-based zero-alloc backend). Builds with no backend dependency.
- [`cjson/`](cjson/) — host tests for the `CjsonBackend`. Requires
  cJSON (pulled via the system package or the bundled vendor copy,
  depending on platform).
- [`nlohmann/`](nlohmann/) — host tests for the `NlohmannBackend`.
  Requires nlohmann/json (header-only, fetched via CMake
  `FetchContent` in this dir's CMakeLists).
- [`softcard/`](softcard/) — PlatformIO project running against a
  mock Notecard simulator (the
  [`note-emu`](https://github.com/m-mcgowan/note-emu) softcard)
  instead of physical hardware. Useful for CI scenarios where real
  Notecard hardware isn't accessible.
- [`shared/`](shared/) — headers shared between `firmware/`,
  `softcard/`, and any future Notecard-backed integration project.
  Defines the common test fixtures and firmware-version gating
  helpers.

## Running

See each sub-directory's README for the specifics. Common patterns:

```bash
# Host backend tests (no hardware)
cmake -B build/integration-buffer -S tests/integration/buffer
cmake --build build/integration-buffer
ctest --test-dir build/integration-buffer

# Hardware tests — needs a named USB device registered with
# `usb-device`; see tests/integration/firmware/README.md
piotest -e serial "MPCB 1.9 Development"
```
