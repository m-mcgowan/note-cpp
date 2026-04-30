# I2C Transport

**Header:** `note/link/i2c.hpp`
**Ported from:** note-c `n_i2c.c`

## Implement `I2cHal`

`I2cHal` is the byte-conduit interface — pure hardware I/O for the
I2C bus. The library wraps your HAL in `link::I2cFramer<>`
(which adds Notecard-specific I2C framing) and then in
`Protocol` (which adds protocol-level CRC, retry, and
session semantics). Either Notecard ctor — sink mode
(`Notecard(transport, alloc)`) or tree mode
(`Notecard(backend, transport)`) — works on the resulting stack;
both go through the unified `ITransact` interface.

```cpp
#include <note/link/i2c.hpp>

class MyI2c : public note::link::I2cHal {
public:
    // Hardware-level I2C reset. Returns false on failure.
    bool     reset()                                          override;

    // Transmit len bytes. Returns false on error (e.g. NACK).
    bool     transmit(const uint8_t* data, size_t len)        override;

    // Receive from the Notecard.
    //   len == 0  priming query: set available = pending byte count, read nothing.
    //   len  > 0  read exactly len bytes; set available = remaining byte count.
    bool     receive(uint8_t* buf, size_t len, uint32_t& available) override;

    uint32_t millis()                                         override;
    void     delay(uint32_t ms)                               override;

    // Optional: override for platforms with a larger I2C buffer.
    // Default is 30 bytes (safe for all Arduino Wire implementations).
    // STM32 / ESP32 can use up to 253 (kI2cMaxMtu).
    size_t   max_transfer()                                   override { return 253; }
};
```

Wire it up:

```cpp
MyI2c hal;
note::link::I2cFramer i2c{hal};
note::Protocol transport{i2c};

// Tree mode (response.body() works) — pass a JsonBackend.
note::backends::CjsonBackend backend;
note::Notecard nc(backend, transport);

// Or sink mode (.into(struct), no JsonBackend needed):
// note::Notecard nc(transport, note::Allocator{});
```

## Arduino

On Arduino, `begin()` handles the full stack:

```cpp
nc.begin(Wire);                       // default pins, default address 0x17
nc.begin(Wire, 0x17);                 // explicit address
nc.begin(Wire, /*sda=*/14, /*scl=*/21);          // custom pins
nc.begin(Wire, /*sda=*/14, /*scl=*/21, 0x17);    // custom pins + address
nc.begin(Wire, note::arduino::external_bus);     // app owns Wire
```

This creates an Arduino `I2cHal` adapter internally.

### Bus management

By default, `note::arduino::I2cHal` "owns" the Wire bus — its constructor
calls `Wire.begin()` and a transient I2C error triggers `Wire.end()` /
`Wire.begin()` inside `reset()`. This is fine on devkits where Wire's
default pins are correct and nothing else shares the bus.

For non-devkit boards or shared buses, pick the right form:

| Form | What the HAL does |
|---|---|
| `nc.begin(Wire)` | calls `Wire.begin()` (no pin args); `reset()` cycles `Wire.end()`/`Wire.begin()` |
| `nc.begin(Wire, sda, scl)` | calls `Wire.begin(sda, scl)`; `reset()` cycles `Wire.end()`/`Wire.begin(sda, scl)` |
| `nc.begin(Wire, note::arduino::external_bus)` | never touches the bus — app calls `Wire.begin(...)` and decides what `reset()` should do |

Use `external_bus` when the bus is shared with other drivers/tasks, or
when the app needs to set non-default Wire properties (e.g.
`setBufferSize()` on ESP32) before `begin()`. In that mode the HAL's
`reset()` is a no-op; the app is responsible for any bus-level recovery
it wants on transient I2C errors.

## Callback variant

```cpp
note::link::I2cCallbackHal hal{
    []() -> bool                                     { /* reset */    return true; },
    [](const uint8_t* d, size_t n) -> bool           { /* transmit */ return true; },
    [](uint8_t* b, size_t n, uint32_t& av) -> bool   { /* receive */  av = 0; return true; },
    []() -> uint32_t                                 { return millis(); },
    [](uint32_t ms)                                  { delay(ms); },
    // optional 6th arg: max_transfer override (default 30)
};
note::link::I2cFramer transport(hal);
```

## Protocol constants

All in `namespace note::link`:

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

## Protocol notes

- **6 ms IO delay** before every I2C operation (`_delayIO` in note-c) —
  empirically required for stability across commercial I2C implementations.
- Request terminated with `\n` (bare newline, not `\r\n` — some Notecard
  firmware versions don't respond to `\r\n` over I2C).
- **Priming query**: before reading a response, `receive(buf, 0, available)`
  is called to learn how many bytes the Notecard has buffered. Actual reads
  request exactly `available` bytes (capped at `max_transfer()`).
- **Chunked receive loop** exits only when `\n` has been received *and*
  `available == 0`. If the Notecard reports more bytes after the `\n`,
  they are drained first.
- **Segment pacing**: after every 250 bytes transmitted, a 250 ms pause
  is inserted to avoid overrunning the Notecard's interrupt buffers.
- **NACK handling**: a transmit failure during reset sync delays 1000 ms
  before retrying.
- CRC behavior is identical to serial (auto-detected, same sequence number
  across retries). See [CRC](transport-crc.md).

## I2C MTU

The default `max_transfer()` is 30 bytes — the limit imposed by the Arduino
Wire library's static 32-byte buffer minus the 2-byte Notecard header.
Platforms with a dynamically-allocated I2C buffer (STM32Duino, most ESP32
boards) can safely use 253. Override `max_transfer()` in your `I2cHal`
subclass or pass the size as the 6th argument to `I2cCallbackHal`.
