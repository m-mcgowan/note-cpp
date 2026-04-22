# `tests/integration/softcard/`

Integration tests against a software-emulated Notecard (the
[`note-emu`](https://github.com/m-mcgowan/note-emu) softcard) rather
than physical hardware. Useful for CI scenarios where a real Notecard
isn't accessible, and for reproducing hardware-specific bugs without
touching a board.

Builds under PlatformIO just like [`firmware/`](../firmware/), but the
transport speaks to the softcard process over an emulated link instead
of real UART/I2C.

## Contents

- `platformio.ini` — PlatformIO environment targeting the softcard
  link.
- `include/` — header wiring specific to the softcard transport.
- `test/` — the test TU and its harness; pulls the portable cases
  from [`../shared/test_notecard_api.cpp`](../shared/).

## Running

See the [`note-emu`](https://github.com/m-mcgowan/note-emu) project
README for softcard setup. With softcard running:

```bash
pio test -d tests/integration/softcard -e softcard
```

This environment is currently exercised manually; wiring it into
GitHub Actions depends on a self-hosted runner with softcard
installed. See `memory/project_wokwi_notecard_chip.md` for related
exploration of a Wokwi-simulator variant.
