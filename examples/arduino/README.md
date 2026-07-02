# `examples/arduino/`

Arduino IDE / PlatformIO / arduino-cli sketches that demonstrate
`note-cpp` on ESP32, Blues Swan (STM32L4+), and other Arduino-framework
targets. Each sub-directory is a single sketch and builds independently.

Every sketch here compiles in CI on every push — `ci.yml`'s
`arduino-cli` job builds all of them against ESP32-S3 + Blues Swan.

## Sketches

- [`quickstart/`](quickstart/) — the sketch the main README points at
  for the "Arduino" quickstart snippet. Minimal "bring up the
  Notecard and configure hub" flow, under ten lines of user code.
- [`serial_basic/`](serial_basic/) — explicit serial transport setup
  with `Serial1`/`Serial2` wiring for boards where the default is
  ambiguous (Swan, ESP32-S3 devkit, etc.).
- [`i2c_basic/`](i2c_basic/) — I2C transport setup showing `Wire`
  begin sequence + pin config for variant boards.
- [`migration/`](migration/) — side-by-side mapping from note-c /
  note-arduino to note-cpp, referenced by
  [`docs/platforms/arduino/migration-from-note-arduino.md`](../../docs/platforms/arduino/migration-from-note-arduino.md).
  Full sketch with a PlatformIO build you can flash and run.
- [`readme_snippets/`](readme_snippets/) — host sketch for every
  `readme:` snippet marker referenced from the main README (fluent
  vs. direct assignment, body-struct, response parsing, etc.). Exists
  so the README's code blocks are real compiled code rather than
  hand-maintained text.

## Building

Via arduino-cli (matches what CI runs):

```bash
for fqbn in \
    esp32:esp32:esp32s3:CDCOnBoot=cdc \
    STMicroelectronics:stm32:Blues:pnum=SWAN_R5 ; do
    arduino-cli compile --fqbn "$fqbn" --library "$PWD" \
        examples/arduino/quickstart
done
```

Via PlatformIO (for `migration/`, which has its own `platformio.ini`):

```bash
pio run -d examples/arduino/migration
```

The root `note-cpp` directory can be referenced as a library either
via `--library "$PWD"` (arduino-cli) or via
`lib_deps = symlink://../..` in a PlatformIO env.
