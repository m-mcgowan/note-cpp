# `tools/binary-size-comparison/`

PlatformIO project that builds the same 8-endpoint Notecard app five
different ways on an Arduino Uno (ATmega328P) and one way on ESP32-S3,
so you can compare flash / RAM footprint between `note-c`, `note-cpp`
at various API layers, and the raw-JSON + `JsonView` tier.

Drives the "How It Scales" table in the main README and the
progression diagram in
[`docs/platforms/arduino/guide.md`](../../docs/platforms/arduino/guide.md#binary-size-comparison).

## Contents

- `platformio.ini` — environments for every comparison row. AVR for
  note-c vs. note-cpp at each API layer, plus an ESP32-S3 parallel
  pair for larger-target reference numbers.
- `src/main_notec.cpp` — baseline using the Blues `note-c` / Arduino
  library directly.
- `src/main_notecpp.cpp` — `note-cpp` typed API (rows 1 – 2 of the
  progression table).
- `src/main_avr_notecpp.cpp` — AVR-specific build toggling
  `NOTE_MINIMAL`, `NOTE_JSONB`, raw `JsonBuf` + `transact_dispatch` +
  `JsonSink`, and `JsonView` / `note::scan` layers (rows 3 – 5).
- `src/main_wokwi_layers.cpp` — same AVR matrix but built for the
  Wokwi simulator target.
- `avr_size_report.sh` — builds every AVR environment and prints a
  formatted flash / RAM / heap comparison table.
- `apply_api_style.py` — toggles between API styles in the source
  files (pre-processor / comment editing) so the same file can be
  built in every row of the table without duplicated source.
- `exclude_avr_libstdcpp_src.py` — PlatformIO pre-script that stops
  PIO from pulling the libstdc++ sources into the AVR build (they're
  not needed and expand compile times significantly).
- `wokwi.toml` / `diagram.json` — Wokwi simulator configuration for
  the Uno board used by `main_wokwi_layers.cpp`.
- `chips/` — Wokwi custom-chip stubs (currently a mock Notecard).

## Mock Notecard limits

`chips/notecard-mock.c` is a *minimal* Notecard stand-in for runtime
verification of the AVR examples. It only canned-responds for three
requests:

| Wire request | Mock response |
|---|---|
| `card.temp` | `{"value":22.5}` |
| `note.template` | `{"bytes":14,"template":true}` |
| `note.get` | `{"body":{"temperature":22.5,"humidity":60}}` |
| anything else | `{}` |

So requests like `card.status` and `card.voltage` come back empty, and
the `JsonView::get_bool("connected")` / `get_double("value")` calls in
the examples extract the field defaults. On Uno that's invisible
because the only `Serial` is owned by the Notecard UART; on the Mega
variant the loop's tail prints the extracted values to the free
debug `Serial`, where you'd see the empty-response defaults reflected.

If you want a richer demo round-trip, extend the chip's request
table or canned values directly — the file is small and the structure
is obvious. Avoid teaching the mock real Notecard semantics; this
harness is for verifying the *transport / render / parse* path, not
the protocol.

## Usage

```bash
# Build every AVR environment and print the comparison table:
tools/binary-size-comparison/avr_size_report.sh

# One specific row:
pio run -d tools/binary-size-comparison -e uno-notecpp-typed
```

Numbers emitted here are sensitive to compiler version and
optimization flags — the checked-in numbers in the main README were
produced with the avr-gcc version PlatformIO pins in this directory's
`platformio.ini`. Rerun the report locally before citing new numbers.
