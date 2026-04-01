# note-cpp Transport Layer

note-cpp ships a complete, header-only implementation of both Notecard wire protocols. These are a direct alternative to note-c's serial and I2C transports — same timing, same CRC, same retry behaviour, no global state, no cJSON dependency.

## Architecture

The transport layer is split into three levels:

```
TransportHal           Pure hardware abstraction (5 primitives)
  ↓
StreamingTransport     Protocol logic: retry, CRC, JSON framing
  ↓
IStreamingTransport    Interface consumed by Notecard
```

**`TransportHal`** (`include/note/transport_hal.hpp`) — the only thing you implement. Five methods: `transmit()`, `read()`, `reset()`, `write_line_terminator()`, `delay()`. No protocol logic, no buffers.

**`StreamingTransport`** (`include/note/streaming_transport.hpp`) — protocol engine over any `TransportHal&`. Handles retry loops, CRC (append on send, verify on receive), and JSON framing. Implements `IStreamingTransport`.

**`IStreamingTransport`** (`include/note/streaming_transport.hpp`) — the interface `Notecard` consumes. Two operations: `transact(build_fn, sink, timeout)` streams a request via a build function and SAX-parses the response into a sink; `send(build_fn)` is fire-and-forget. Also exposes raw `write()`/`read()` for binary (COBS) streaming.

```
include/note/
    transport_hal.hpp          TransportHal (pure HAL)
    streaming_transport.hpp    IStreamingTransport, StreamingTransport

include/note/transport/
    serial.hpp          SerialHal, SerialCallbackHal, NotecardSerial
    i2c.hpp             I2CHal,    I2cCallbackHal,    NotecardI2c
    detail/crc32.hpp    CRC32, crc_add, crc_check_and_strip  (internal)
```

`NotecardSerial` implements `TransportHal` — it adapts a `SerialHal` (your hardware) into the `TransportHal` interface that `StreamingTransport` consumes. `NotecardI2c` extends `AbstractTransport` (the buffered path, see below).

Auto-reset on first use, CRC auto-detection, segmented TX, and retry logic are handled by `StreamingTransport`, not the HAL.

### Buffered path (backward compatibility)

For testing or platforms that prefer buffered I/O, the original `IBufferedTransport` (renamed from `ITransport`, backward-compat alias kept) and `AbstractTransport` remain available. `CallbackTransport` wraps lambdas as an `IBufferedTransport` for test harnesses. `NotecardI2c` still extends `AbstractTransport`.

```cpp
// Buffered path — for tests and backward compat
note::Notecard nc(backend, buffered_transport);

// Streaming path — zero buffer, zero heap
note::Notecard nc(streaming_transport, allocator);
```

---

## Serial transport

**Header:** `note/transport/serial.hpp`
**Ported from:** note-c `n_serial.c` + `n_request.c`

### Implement `SerialHal`

```cpp
#include <note/transport/serial.hpp>

class MySerial : public note::transport::SerialHal {
public:
    // Send all len bytes. Returns false on hardware error.
    bool     transmit(const uint8_t* data, size_t len) override { /* write to UART */ }

    // Non-blocking read. Return 0..max_len bytes currently available.
    // Must not block waiting for more data.
    size_t   receive(uint8_t* buf, size_t max_len)     override { /* non-blocking UART read */ }

    // Monotonic millisecond counter (wraps after ~49 days, like Arduino millis()).
    uint32_t millis()                                  override { return ::millis(); }

    // Block for exactly ms milliseconds.
    void     delay(uint32_t ms)                        override { ::delay(ms); }
};

MySerial hal;
note::transport::NotecardSerial serial_hal(hal);  // implements TransportHal
note::StreamingTransport transport(serial_hal);    // protocol logic
note::Notecard nc(transport, allocator);           // streaming path
```

`NotecardSerial` implements `TransportHal` (not `AbstractTransport`). It adapts the four `SerialHal` primitives into `TransportHal`'s five methods — `transmit()`, `read()` (blocking with timeout), `reset()`, `write_line_terminator()` (`\r\n`), and `delay()`. Protocol logic (retry, CRC, JSON framing) is handled by `StreamingTransport`.

### Callback variant

For tests or host-side tooling where subclassing is unnecessary:

```cpp
note::transport::SerialCallbackHal hal{
    [](const uint8_t* d, size_t n) -> bool  { /* transmit */ return true; },
    [](uint8_t* buf, size_t max) -> size_t  { /* receive  */ return 0;    },
    []() -> uint32_t                        { return millis(); },
    [](uint32_t ms)                         { delay(ms); },
};
note::transport::NotecardSerial serial_hal(hal);
note::StreamingTransport transport(serial_hal);
```

### Protocol constants

All in `namespace note::transport`:

| Constant | Value | note-c equivalent |
|---|---|---|
| `kSerialSegmentMaxLen` | 250 bytes | `CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN` |
| `kSerialSegmentDelayMs` | 250 ms | `CARD_REQUEST_SERIAL_SEGMENT_DELAY_MS` |
| `kIntraTransactionTimeoutMs` | 1000 ms | `CARD_INTRA_TRANSACTION_TIMEOUT_SEC * 1000` |
| `kResetDrainMs` | 500 ms | `CARD_RESET_DRAIN_MS` |
| `kResetSyncRetries` | 10 | `CARD_RESET_SYNC_RETRIES` |
| `kMaxRetries` | 5 | — |
| `kRetryDelayMs` | 500 ms | — |

### Protocol notes

- Request terminated with `\r\n` (Notecard serial protocol requirement).
- Reset sequence: send `\n`, drain until only `\r`/`\n` received for 500 ms.
- TX segments: each chunk ≤ 250 bytes; 250 ms pause between chunks.
- RX: poll `receive()` in a tight loop until `\n` seen. After first byte, switches to 1-second intra-transaction timeout.
- CRC auto-detected: first response containing a `"crc"` field enables CRC for subsequent requests.
- CRC sequence number is fixed for all retries of a given request (matches note-c behaviour).

### Binary streaming

`IStreamingTransport::write()`/`read()` pass through to `TransportHal::transmit()`/`read()` for raw binary (COBS) data, bypassing JSON framing and CRC.
On serial, `\n` is the frame delimiter for both JSON responses and COBS streams.

---

## I2C transport

**Header:** `note/transport/i2c.hpp`
**Ported from:** note-c `n_i2c.c`

### Implement `I2CHal`

`NotecardI2c` currently extends `AbstractTransport` (the buffered path), so it is used with the buffered `Notecard` constructor. A `TransportHal` migration is planned.

```cpp
#include <note/transport/i2c.hpp>

class MyI2c : public note::transport::I2CHal {
public:
    // Hardware-level I2C reset. Returns false on failure.
    bool     reset()                                          override { /* assert/deassert reset pin */ }

    // Transmit len bytes. Returns false on error (e.g. NACK).
    bool     transmit(const uint8_t* data, size_t len)        override { /* Wire.write */ }

    // Receive from the Notecard.
    //   len == 0  priming query: set available = pending byte count, read nothing.
    //   len  > 0  read exactly len bytes; set available = remaining byte count.
    bool     receive(uint8_t* buf, size_t len, uint32_t& available) override { /* Wire.requestFrom */ }

    uint32_t millis()                                         override { return ::millis(); }
    void     delay(uint32_t ms)                               override { ::delay(ms); }

    // Optional: override for platforms with a larger I2C buffer.
    // Default is 30 bytes (safe for all Arduino Wire implementations).
    // STM32 / ESP32 can use up to 253 (kI2cMaxMtu).
    size_t   max_transfer()                                   override { return 253; }
};

MyI2c hal;
note::transport::NotecardI2c transport(hal);
note::Notecard nc(backend, transport);  // buffered path (IBufferedTransport)
```

### Callback variant

```cpp
note::transport::I2cCallbackHal hal{
    []() -> bool                                     { /* reset */    return true; },
    [](const uint8_t* d, size_t n) -> bool           { /* transmit */ return true; },
    [](uint8_t* b, size_t n, uint32_t& av) -> bool   { /* receive */  av = 0; return true; },
    []() -> uint32_t                                 { return millis(); },
    [](uint32_t ms)                                  { delay(ms); },
    // optional 6th arg: max_transfer override (default 30)
};
note::transport::NotecardI2c transport(hal);
// Use with buffered Notecard constructor: Notecard nc(backend, transport);
```

### Protocol constants

All in `namespace note::transport`:

| Constant | Value | note-c equivalent |
|---|---|---|
| `kI2cDefaultAddress` | `0x17` | `NOTE_I2C_ADDR_DEFAULT` |
| `kI2cDefaultMtu` | 30 bytes | `NOTE_I2C_MTU_DEFAULT` |
| `kI2cMaxMtu` | 253 bytes | `NOTE_I2C_MTU_MAX` |
| `kI2cIoDelayMs` | 6 ms | `_delayIO()` |
| `kI2cSegmentMaxLen` | 250 bytes | `CARD_REQUEST_I2C_SEGMENT_MAX_LEN` |
| `kI2cSegmentDelayMs` | 250 ms | `CARD_REQUEST_I2C_SEGMENT_DELAY_MS` |
| `kI2cChunkDelayMs` | 20 ms | `CARD_REQUEST_I2C_CHUNK_DELAY_MS` |
| `kI2cNackWaitMs` | 1000 ms | `CARD_REQUEST_I2C_NACK_WAIT_MS` |
| `kI2cResetDrainMs` | 500 ms | `CARD_RESET_DRAIN_MS` |
| `kI2cResetSyncRetries` | 10 | `CARD_RESET_SYNC_RETRIES` |
| `kI2cResponsePollMs` | 50 ms | poll interval in `_i2cNoteQueryLength` |
| `kI2cIntraTimeoutMs` | 1000 ms | `CARD_INTRA_TRANSACTION_TIMEOUT_SEC * 1000` |
| `kI2cMaxRetries` | 5 | — |
| `kI2cRetryDelayMs` | 500 ms | — |

### Protocol notes

- **6 ms IO delay** before every I2C operation (`_delayIO` in note-c) — empirically required for stability across commercial I2C implementations.
- Request terminated with `\n` (bare newline, not `\r\n` — some Notecard firmware versions don't respond to `\r\n` over I2C).
- **Priming query**: before reading a response, `receive(buf, 0, available)` is called to learn how many bytes the Notecard has buffered. Actual reads request exactly `available` bytes (capped at `max_transfer()`).
- **Chunked receive loop** exits only when `\n` has been received *and* `available == 0`. If the Notecard reports more bytes after the `\n`, they are drained first.
- **Segment pacing**: after every 250 bytes transmitted, a 250 ms pause is inserted to avoid overrunning the Notecard's interrupt buffers.
- **NACK handling**: a transmit failure during reset sync delays 1000 ms before retrying.
- CRC behaviour is identical to serial (auto-detected, same sequence number across retries).

### I2C MTU

The default `max_transfer()` is 30 bytes — the limit imposed by the Arduino Wire library's static 32-byte buffer minus the 2-byte Notecard header. Platforms with a dynamically-allocated I2C buffer (STM32Duino, most ESP32 boards) can safely use 253. Override `max_transfer()` in your `I2CHal` subclass or pass the size as the 6th argument to `I2cCallbackHal`.

---

## CRC

CRC is handled at two levels, depending on the transport path:

**Streaming path** (`StreamingTransport`): CRC is accumulated incrementally. On send, a `CrcWriter` wraps the `JsonWriter` and appends the CRC suffix after the closing brace. On receive, a `CrcAccumulator` feeds on bytes as they arrive, and a `CrcFieldSink` in the SAX sink chain extracts the CRC field. `StreamingTransport` compares accumulated vs extracted values.

**Buffered path** (`AbstractTransport`): CRC uses the in-place buffer functions from `note/transport/detail/crc32.hpp` — `crc_add()` appends CRC to the wire buffer, `crc_check_and_strip()` validates and removes it from the response.

Both paths share the same CRC behavior:

- **Auto-detected**: if the Notecard includes a `"crc"` field in any response, CRC is enabled for all subsequent requests in that session.
- **Format**: `,"crc":"SSSS:CCCCCCCC"` — 22 bytes appended before the closing `}`. `SSSS` is a 4-hex-digit sequence number; `CCCCCCCC` is the CRC32 of the JSON body (without the CRC field, without the trailing newline).
- Error responses (`"err"` field present) bypass CRC validation, matching note-c behaviour.

The CRC implementation and test suite are ported directly from note-c and track upstream changes. See `tests/test_transport_crc32.cpp` for the full test coverage mapping.
