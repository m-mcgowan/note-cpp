# `tests/integration/shared/`

Shared test fixtures and helpers used by every Notecard-backed
integration project (`firmware/`, `softcard/`, any future variants).
No build artifacts here; this dir is include-only.

## Contents

- `notecard_api_fixture.hpp` — the common `note::Api<>` fixture
  instantiation, plus firmware-version gating helpers
  (`build_version_excludes`, `parse_firmware_version`) that let
  each test project skip test cases that don't apply to the
  connected Notecard's firmware level.
- `test_notecard_api.cpp` — the portable test cases run against any
  real Notecard endpoint. Both `firmware/` (real hardware) and
  `softcard/` (simulator) compile this file via their own
  `platformio.ini` `build_src_filter` / symlink.
