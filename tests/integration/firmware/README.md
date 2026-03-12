# Integration Tests

Hardware integration tests that exercise `note-cpp` against a real Notecard over serial UART and I2C. Built with PlatformIO targeting an ESP32-S3.

## Requirements

- **Notecard** connected to the ESP32-S3 via both serial and I2C
- **PlatformIO** (`pip install platformio` or VS Code extension)
- **ESP32-S3 DevKitC** (or compatible board — adjust pins as needed)

## Pin configuration

Source `env.sh` before building. It injects pin definitions via `PLATFORMIO_BUILD_FLAGS`.

```bash
# ESP32-S3 DevKitC defaults (TX=17, RX=18, SDA=1, SCL=2)
source env.sh

# Custom pins
source env.sh --rx=21 --tx=47 --sda=39 --scl=38
```

## Running

```bash
source env.sh
pio test -e esp32s3            # run all tests
pio test -e esp32s3 -f serial  # serial tests only
pio test -e esp32s3 -f i2c    # I2C tests only
```

## Test coverage

Each transport (serial and I2C) runs the same test suite:

| Test | What it exercises |
|------|-------------------|
| **card.version** | Basic request/response, device info fields |
| **hub.set + hub.get** | Configuration write then read-back |
| **note.add** | Fire-and-forget note creation |
| **note.add + note.get body** | Typed body struct round-trip (cJSON serialization) |
| **note.changes** | Change tracker with reset/add/query cycle |
| **env.default set + get** | Environment variable round-trip |
| **card.binary put + get** | Binary data round-trip with COBS encoding and MD5 verification |
| **Bad request** | Notecard error surfacing (`Error::Notecard`) |

### Binary data test

The `card.binary` tests exercise the binary data transfer protocol, which differs from
the standard JSON request/response cycle:

1. `card.binary` — query available space
2. `card.binary.put` — JSON handshake (cobs size + MD5)
3. Raw COBS-encoded bytes sent via transport HAL
4. `card.binary` — verify stored data (length, cobs size)
5. `card.binary.get` — JSON handshake (request retrieval)
6. Raw COBS-encoded bytes received via transport HAL
7. Decode and verify data matches original

## Stack

Each test builds the full `note-cpp` stack:

```
doctest runner
  └─ note::Api (typed requests)
       └─ note::Notecard (JSON backend + transport)
            ├─ CjsonBackend (cJSON, bundled with ESP-IDF)
            └─ NotecardSerial / NotecardI2c (wire protocol)
                 └─ Esp32SerialHal / Esp32I2cHal (Arduino HAL)
```

## Helpers

| File | Purpose |
|------|---------|
| `include/cobs.hpp` | COBS encoder/decoder for binary data tests |
| `include/md5.hpp` | MD5 hex digest via mbedtls (for `card.binary.put` checksum) |
| `include/hal_serial.hpp` | ESP32 serial HAL implementation |
| `include/hal_i2c.hpp` | ESP32 I2C HAL implementation |
