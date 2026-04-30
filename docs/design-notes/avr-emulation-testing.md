# AVR Emulation Testing — Wokwi Mock Notecard

## Goal

Run the AVR binary size comparison example on an emulated Arduino Uno
with a mock Notecard, proving the firmware works end-to-end — not just
compiles.

## Approach: Wokwi + Custom Chip

[Wokwi](https://wokwi.com) simulates Arduino boards and supports custom
chips written in C (compiled to WASM). A custom chip acts as a mock
Notecard on the Uno's serial pins, responding to requests with
canned responses. The mock speaks both JSON and JSONB — it detects the
format from the request header and responds in kind.

```
Arduino Uno (QEMU inside Wokwi)
    |
    +-- Pin 1 (TX) --> notecard-mock RX
    +-- Pin 0 (RX) <-- notecard-mock TX
    |
    +-- Firmware: main_avr_notecpp.cpp
         NOTE_MINIMAL builds use JSONB automatically (NOTE_JSONB=1)
         Non-MINIMAL builds use JSON text
```

### What the firmware does

1. `setup()`: `hub.set` (configure), `note.template` (register template)
2. `loop()`: `card.temp` (read), `note.add` (send body), `note.get` (receive body)

### What the mock handles

| Request | Response |
|---------|----------|
| `hub.set` | `{}` (empty success) |
| `note.template` | `{"bytes":14,"template":true}` |
| `card.temp` | `{"value":22.5}` |
| `note.add` | `{}` (empty success) |
| `note.get` | `{"body":{"temperature":22.5,"humidity":60}}` |

For JSON requests, the mock matches on the `"req"` string value.
For JSONB requests, the mock COBS-decodes the payload, walks JSONB
opcodes to find the `req` field, then builds a JSONB response with
the same canned data.

## Files

| File | Purpose |
|------|---------|
| `chips/notecard-mock.c` | Mock chip source (JSON + JSONB) |
| `chips/notecard-mock.json` | Wokwi chip metadata (name, pins) |
| `chips/notecard-mock.wasm` | Compiled WASM binary (checked in) |
| `chips/wokwi-api.h` | Wokwi custom chip API header |
| `diagram.json` | Wokwi circuit diagram (Uno + mock chip wiring) |
| `wokwi.toml` | Wokwi config (firmware path, chip binary) |
| `wokwi-test.sh` | Test runner script (builds, runs, checks output) |

## Setup steps

### 1. Install wokwi-cli

```bash
curl -L https://wokwi.com/ci/install.sh | sh
```

### 2. Get a Wokwi token

1. Go to https://wokwi.com/dashboard/ci
2. Create a token
3. `export WOKWI_CLI_TOKEN=<token>`

Free tier: 50 min/month simulation time.

### 3. Compile the custom chip

```bash
cd tools/binary-size-comparison
~/.wokwi/bin/wokwi-cli chip compile chips/notecard-mock.c
# Downloads WASI-SDK automatically on first run
# Outputs: chips/notecard-mock.wasm
```

The compiled `.wasm` is checked into the repo so most users don't need
this step. Recompile after editing `notecard-mock.c`.

### 4. Build the firmware

```bash
pio run -e wokwi-layer4
```

Or any of the layered test environments:

| Environment | What it tests |
|-------------|---------------|
| `wokwi-layer1` | SerialHal transmit + receive |
| `wokwi-layer2` | SerialFramer reset handshake |
| `wokwi-layer3` | Protocol transact |
| `wokwi-layer4` | Full Api.execute with typed response parsing |

### 5. Run the simulation

```bash
~/.wokwi/bin/wokwi-cli . --timeout 10000 --serial-log-file serial.log
```

Or use the test runner script:

```bash
./wokwi-test.sh wokwi-layer4 "PASS L4" 30000
```

The script builds the firmware, generates `wokwi.toml`, runs the
simulation, and checks for the expected output string.

### 6. Run from VS Code

Open `tools/binary-size-comparison/` in VS Code with the Wokwi
extension installed. Build the firmware (`pio run -e wokwi-layer4`),
then use the Wokwi extension to start the simulation.

## JSONB wire format

`NOTE_MINIMAL` builds (including all AVR targets) automatically use
JSONB (`NOTE_JSONB=1`). The mock Notecard chip detects JSONB requests
by the `{:` header prefix and responds in JSONB:

- JSON request: starts with `{"`  -> JSON response
- JSONB request: starts with `{:` -> JSONB response (COBS-framed)

To test with JSON instead of JSONB on a MINIMAL build, add
`-DNOTE_JSONB=0` to the environment's `build_flags`.

## CI integration (GitHub Actions)

```yaml
jobs:
  avr-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: pip install platformio
      - run: cd tools/binary-size-comparison && pio run -e wokwi-layer4
      - uses: wokwi/wokwi-ci-server-action@v1
      - uses: wokwi/wokwi-ci-action@v1
        with:
          token: ${{ secrets.WOKWI_CLI_TOKEN }}
          path: tools/binary-size-comparison
          timeout: 15000
```

The `wokwi-ci-server-action` runs simulation locally (Docker) to avoid
burning cloud quota.
