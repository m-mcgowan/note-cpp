# AVR Emulation Testing — Wokwi Mock Notecard

## Goal

Run the AVR binary size comparison example on an emulated Arduino Uno
with a mock Notecard, proving the firmware works end-to-end — not just
compiles.

## Approach: Wokwi + Custom Chip

[Wokwi](https://wokwi.com) simulates Arduino boards and supports custom
chips written in C (compiled to WASM). A custom chip acts as a mock
Notecard on the Uno's serial pins, responding to JSON requests with
canned responses.

```
Arduino Uno (QEMU inside Wokwi)
    │
    ├── Pin 1 (TX) ──→ notecard-mock RX
    ├── Pin 0 (RX) ←── notecard-mock TX
    │
    └── Firmware: main_avr_notecpp.cpp
         sends: {"req":"hub.set","product":"com.example.size-test",...}\r\n
         expects: {}\r\n
```

### What the firmware does

1. `setup()`: `hub.set` (configure), `note.template` (register template)
2. `loop()`: `card.temp` (read), `note.add` (send body), `note.get` (receive body)

### What the mock needs to handle

| Request | Response |
|---------|----------|
| `hub.set` | `{}` |
| `note.template` | `{"bytes":14,"template":true}` |
| `card.temp` | `{"value":22.5}` |
| `note.add` | `{}` |
| `note.get` | `{"payload":"...","body":{"temperature":22.5,"humidity":60}}` |

Pattern matching on `"req":"card.temp"` etc. is sufficient — no full
JSON parser needed in the mock.

## Files to create

### `examples/binary-size-comparison/chips/notecard-mock.c`

Mock Notecard custom chip (~80 lines of C):

```c
#include "wokwi-api.h"
#include <string.h>
#include <stdio.h>

typedef struct {
    uart_dev_t uart;
    char rx_buf[256];
    size_t rx_pos;
} chip_state_t;

static chip_state_t state;

static const char* match_response(const char* req) {
    if (strstr(req, "\"card.temp\""))
        return "{\"value\":22.5}\r\n";
    if (strstr(req, "\"note.template\""))
        return "{\"bytes\":14,\"template\":true}\r\n";
    if (strstr(req, "\"note.get\""))
        return "{\"body\":{\"temperature\":22.5,\"humidity\":60}}\r\n";
    // Default: empty success
    return "{}\r\n";
}

static void on_rx_byte(void *user_data, uint8_t byte) {
    chip_state_t *s = user_data;
    if (s->rx_pos < sizeof(s->rx_buf) - 1) {
        s->rx_buf[s->rx_pos++] = byte;
    }
    if (byte == '\n') {
        s->rx_buf[s->rx_pos] = '\0';
        const char* response = match_response(s->rx_buf);
        uart_write(s->uart, (const uint8_t*)response, strlen(response));
        s->rx_pos = 0;
    }
}

void chip_init(void) {
    const uart_config_t cfg = {
        .tx = pin_init("TX", OUTPUT),
        .rx = pin_init("RX", INPUT),
        .baud_rate = 9600,
        .rx_data = on_rx_byte,
        .user_data = &state,
    };
    state.uart = uart_init(&cfg);
}
```

### `examples/binary-size-comparison/chips/notecard-mock.chip.json`

```json
{
  "name": "Mock Notecard",
  "author": "note-cpp",
  "pins": ["VCC", "GND", "TX", "RX"]
}
```

### `examples/binary-size-comparison/diagram.json`

```json
{
  "version": 1,
  "author": "note-cpp",
  "editor": "wokwi",
  "parts": [
    { "id": "uno", "type": "wokwi-arduino-uno", "top": 160, "left": 20 },
    { "id": "notecard", "type": "chip-notecard-mock", "top": 0, "left": 200 }
  ],
  "connections": [
    ["uno:1", "notecard:RX", "green", []],
    ["uno:0", "notecard:TX", "yellow", []],
    ["uno:GND.1", "notecard:GND", "black", []],
    ["uno:5V", "notecard:VCC", "red", []]
  ]
}
```

### `examples/binary-size-comparison/wokwi.toml`

```toml
[wokwi]
version = 1
firmware = '.pio/build/avr-notecpp/firmware.hex'
elf = '.pio/build/avr-notecpp/firmware.elf'

[[chip]]
name = 'notecard-mock'
binary = 'chips/notecard-mock.chip.wasm'
```

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
cd examples/binary-size-comparison
wokwi-cli chip compile chips/notecard-mock.c
# Downloads WASI-SDK automatically, outputs chips/notecard-mock.chip.wasm
```

### 4. Build the firmware

```bash
pio run -e avr-notecpp
```

### 5. Run the simulation

```bash
wokwi-cli . --timeout 10000 --serial-log-file serial.log
```

The firmware runs in the simulated Uno, sends requests to the mock
Notecard, receives responses. Serial output is captured in `serial.log`.

### 6. Verify (optional automation)

Create `test.yaml`:
```yaml
name: 'AVR Integration Test'
version: 1
steps:
  - wait-serial: '{"req":"hub.set"'
  - wait-serial: '{"req":"note.add"'
  - wait-serial: '{"req":"note.get"'
```

Run with: `wokwi-cli . --scenario test.yaml --timeout 15000`

## CI integration (GitHub Actions)

```yaml
jobs:
  avr-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: pip install platformio
      - run: cd examples/binary-size-comparison && pio run -e avr-notecpp
      - uses: wokwi/wokwi-ci-server-action@v1
      - uses: wokwi/wokwi-ci-action@v1
        with:
          token: ${{ secrets.WOKWI_CLI_TOKEN }}
          path: examples/binary-size-comparison
          timeout: 15000
          scenario: test.yaml
```

The `wokwi-ci-server-action` runs simulation locally (Docker) to avoid
burning cloud quota.

## Alternatives considered

**QEMU AVR** (`qemu-system-avr -machine uno`): Available locally, no
token needed. But limited peripheral emulation — no built-in way to
mock a bidirectional serial device. Would require a custom QEMU chardev
backend or external pipe plumbing.

**SimAVR**: Open-source, fully local. Supports UART hooks from C code.
More flexible than QEMU but requires compilation. Not installed.

## Firmware modifications needed

The current AVR example has an infinite `loop()` with `delay(60000)`.
For testing, we need the firmware to:
1. Run through setup + one loop iteration
2. Print a success marker (e.g., `"PASS"` to a separate output)
3. Halt (or enter a short loop that Wokwi's timeout catches)

Since the Uno only has one serial (shared with Notecard), the success
marker can't go to Serial. Options:
- Use a GPIO pin as a "done" flag (Wokwi can monitor pin state)
- Add a `SoftwareSerial` on other pins for test output
- Just verify the mock received the expected requests (the test
  scenario watches serial traffic in both directions)

The simplest: the test scenario watches for all expected requests in
the serial output. If the firmware sends all 5 request types, it works.
