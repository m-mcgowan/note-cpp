# Integration Tests

Hardware integration tests that exercise `note-cpp` against a real Notecard over serial UART and/or I2C. Built with PlatformIO targeting an ESP32-S3.

## Requirements

- **Notecard** connected to the ESP32-S3 via serial and/or I2C
- **PlatformIO** (`pip install platformio` or VS Code extension)
- **ESP32-S3 DevKitC** (or compatible board — adjust pins as needed)

## Interface selection

Tests compile conditionally based on which pin macros are defined. Define serial pins (`NOTECARD_SERIAL_RX`/`TX`) to enable serial tests, I2C pins (`NOTECARD_I2C_SDA`/`SCL`) to enable I2C tests. If neither is defined, the build errors.

Three PlatformIO environments are provided:

| Environment | Interfaces | Command |
|-------------|-----------|---------|
| `serial` | Serial only | `pio test -e serial` |
| `i2c` | I2C only | `pio test -e i2c` |
| `both` | Serial + I2C | `pio test -e both` |

## Pin configuration

### VS Code / PlatformIO IDE

Edit the pin values in `platformio.ini` under `[pins:serial]` and `[pins:i2c]`:

```ini
[pins:serial]
build_flags =
    -DNOTECARD_SERIAL_RX=38
    -DNOTECARD_SERIAL_TX=39

[pins:i2c]
build_flags =
    -DNOTECARD_I2C_SDA=14
    -DNOTECARD_I2C_SCL=21
```

Then select the desired environment (`serial`, `i2c`, or `both`) from the PlatformIO environment selector.

### Command line

Source `env.sh` or `boards.sh` before building. Environment variables override `platformio.ini` defaults via the `set_pins.py` pre-build script.

```bash
# Board presets
source boards.sh 1.9                        # Both interfaces, v1.9 pins
source boards.sh 1.9 --i2c-only             # I2C only, v1.9 pins

# Custom pins
source env.sh --rx=21 --tx=47 --sda=39 --scl=38   # Both, custom pins
source env.sh --i2c-only --sda=14 --scl=21         # I2C only
source env.sh --serial-only                         # Serial only, defaults

# Run
pio test -e i2c
pio test -e both
```

## CI

`ci.sh` verifies all environments build correctly and the no-interface guard fires:

```bash
./ci.sh              # build-only (no hardware needed)
./ci.sh --test       # build + upload + run on hardware
./ci.sh --test --upload-port /dev/cu.usbmodem...  # explicit port
```

## Test coverage

Each transport runs the same test suite:

| Test | What it exercises |
|------|-------------------|
| **card.version** | Basic request/response, device info fields |
| **card.status** | Operational state query |
| **hub.set + hub.get** | Configuration write then read-back |
| **note.add** | Fire-and-forget note creation |
| **note.add + note.get body** | Typed body struct round-trip (cJSON serialization) |
| **note.changes** | Change tracker with reset/add/query cycle |
| **env.default set + get** | Environment variable round-trip |
| **card.binary — text** | Binary round-trip with plain text payload |
| **card.binary — zeros** | Binary round-trip with data containing zero bytes (COBS edge case) |
| **card.binary — 512B** | Binary round-trip with large payload (forces I2C multi-chunk transfer) |
| **Bad request** | Notecard error surfacing (`Error::Notecard`) |

### Binary data tests

The `card.binary` tests exercise the binary data transfer protocol, which differs from
the standard JSON request/response cycle. After `card.binary.put` / `card.binary.get`,
raw COBS-encoded bytes are sent/received directly on the wire — outside the normal JSON
transport layer.

Three payload variants test different aspects:

- **Text** — basic round-trip with no special bytes
- **Zeros** — data containing zero bytes every 5th position; COBS encoding exists
  specifically to handle zeros, so this verifies the encoder/decoder
- **512B** — exceeds the I2C `max_transfer()` size (253 bytes), forcing multiple
  chunked I2C transactions for both transmit and receive

Each variant follows the same lifecycle:

1. `card.binary` — query available space
2. `card.binary.put` — JSON handshake (COBS size + MD5)
3. Raw COBS-encoded bytes sent via transport HAL (chunked for I2C)
4. `card.binary` — verify stored data (length, COBS size, MD5)
5. `card.binary.get` — JSON handshake (request retrieval, verify MD5)
6. Raw COBS-encoded bytes received via transport HAL (chunked for I2C)
7. Decode and verify data matches original

The I2C tests use dedicated `i2c_binary_transmit()` / `i2c_binary_receive()` helpers
that chunk data into `max_transfer()`-sized I2C transactions with IO pacing delays —
matching the chunking that `NotecardI2c` uses for JSON.

## Stack

Each test builds the full `note-cpp` stack:

```
doctest runner
  └─ note::Api (typed requests)
       └─ note::Notecard (JSON backend + transport)
            ├─ CjsonBackend (cJSON, bundled with ESP-IDF)
            └─ NotecardSerial / NotecardI2c (wire protocol)
                 └─ Esp32SerialHal / Esp32I2CHal (Arduino HAL)
```

## How conditional compilation works

Pin definitions control compilation via a chain of `#ifdef` guards:

1. `hal_serial.hpp` / `hal_i2c.hpp` — if pin macros are defined, sets `NOTECARD_TEST_SERIAL` / `NOTECARD_TEST_I2C` and provides the HAL class
2. `test_serial.cpp` / `test_i2c.cpp` — wrapped in `#ifdef NOTECARD_TEST_SERIAL` / `#ifdef NOTECARD_TEST_I2C`
3. `main.cpp` — includes both HAL headers and emits `#error` if neither interface is configured

The `set_pins.py` PlatformIO pre-build script allows `env.sh` environment variables to override `platformio.ini` defaults without macro redefinition warnings.

## Helpers

| File | Purpose |
|------|---------|
| `include/cobs.hpp` | COBS encoder/decoder for binary data tests |
| `include/md5.hpp` | MD5 hex digest via mbedtls (for `card.binary.put` checksum) |
| `include/hal_serial.hpp` | ESP32 serial HAL (guarded by `NOTECARD_SERIAL_RX`/`TX`) |
| `include/hal_i2c.hpp` | ESP32 I2C HAL (guarded by `NOTECARD_I2C_SDA`/`SCL`) |
| `set_pins.py` | PlatformIO pre-build script for env var pin overrides |
