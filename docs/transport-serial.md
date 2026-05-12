# Serial transport

**Header:** `note/link/serial.hpp`
**Ported from:** note-c `n_serial.c` + `n_request.c`

## Implement `SerialHal`

Subclass `note::link::SerialHal` with your platform's UART driver:

```cpp
#include <note/link/serial.hpp>

class MySerial : public note::link::SerialHal {
public:
    // Send all len bytes. Returns false on hardware error.
    bool     transmit(const uint8_t* data, size_t len) override;

    // Non-blocking read. Return 0..max_len bytes currently available.
    // Must not block waiting for more data.
    size_t   receive(uint8_t* buf, size_t max_len)     override;

    // Monotonic millisecond counter (wraps after ~49 days).
    uint32_t millis()                                  override;

    // Block for exactly ms milliseconds.
    void     delay(uint32_t ms)                        override;
};
```

Wire it up:

```cpp
MySerial hal;
note::link::SerialFramer serial_hal(hal);  // implements Hal
note::Protocol transport(serial_hal);              // protocol logic
note::Notecard nc(transport, allocator);           // streaming path
```

`SerialFramer` adapts the four `SerialHal` primitives into `Hal`'s
five methods — `transmit()`, `read()` (blocking with timeout), `reset()`,
`write_line_terminator()` (`\r\n`), and `delay()`. Protocol logic (CRC,
JSON framing, retry) is handled by `Protocol` and `Notecard`.

## Arduino

On Arduino, `begin()` handles the full stack:

```cpp
nc.begin(Serial1, 9600);
```

This creates an Arduino `SerialHal` adapter internally.

## Callback variant

For tests or host-side tooling where subclassing is unnecessary:

```cpp
note::link::SerialCallbackHal hal{
    [](const uint8_t* d, size_t n) -> bool  { /* transmit */ return true; },
    [](uint8_t* buf, size_t max) -> size_t  { /* receive  */ return 0;    },
    []() -> uint32_t                        { return millis(); },
    [](uint32_t ms)                         { delay(ms); },
};
note::link::SerialFramer serial_hal(hal);
note::Protocol transport(serial_hal);
```

## Protocol constants

All in `namespace note::link`:

| Constant | Value | note-c equivalent |
|---|---|---|
| `kSerialSegmentMaxLen` | 250 bytes | `CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN` |
| `kSerialSegmentDelayMs` | 250 ms | `CARD_REQUEST_SERIAL_SEGMENT_DELAY_MS` |
| `kIntraTransactionTimeoutMs` | 1000 ms | `CARD_INTRA_TRANSACTION_TIMEOUT_SEC * 1000` |
| `kResetDrainMs` | 500 ms | `CARD_RESET_DRAIN_MS` |
| `kResetSyncRetries` | 10 | `CARD_RESET_SYNC_RETRIES` |

## Protocol notes

- Request terminated with `\r\n` (Notecard serial protocol requirement).
- Reset sequence: send `\n`, drain until only `\r`/`\n` received for 500 ms.
- TX segments: each chunk ≤ 250 bytes; 250 ms pause between chunks.
- RX: poll `receive()` in a tight loop until `\n` seen. After first byte,
  switches to 1-second intra-transaction timeout.
- CRC auto-detected on first response containing a `"crc"` field. See
  [CRC](transport-crc.md) for details.
- CRC sequence number is fixed for all retries of a given request
  (matches note-c behavior).

## Binary streaming

`ITransact::write()`/`read()` pass through to
`Hal::transmit()`/`read()` for raw binary (COBS) data, bypassing
JSON framing and CRC. On serial, `\n` is the frame delimiter for both JSON
responses and COBS streams.

See [Binary Transfer](binary-transfer.md) for the full protocol.
