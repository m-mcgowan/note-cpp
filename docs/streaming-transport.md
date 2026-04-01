# Streaming Transport

The transport layer has two interfaces — `IStreamingTransport` (streaming,
zero-buffer) and `IBufferedTransport` (buffered, for tests/compat). The
streaming path is the primary design; the buffered path exists for backward
compatibility and test harnesses.

## Transport Interfaces

### IStreamingTransport (primary)

```
IStreamingTransport
  transact(build_fn, sink, timeout) → Result<void>  // stream request, SAX-parse response
  send(build_fn) → Result<void>                     // fire-and-forget
  write(data, len) → Result<void>                   // raw byte write (binary/COBS)
  read(buf, max_len, timeout) → Result<size_t>      // raw byte read (binary/COBS)
  reset()                                           // drain + resync
  abort()
```

Requests are streamed via a build function (`BuildFn`) that writes JSON
fields into a `JsonBuilder&`. Responses are SAX-parsed directly from the
wire into a `JsonSink&`. No intermediate buffers.

### IBufferedTransport (backward compat)

```
IBufferedTransport (was ITransport)
  transact(request, timeout) → Result<string_view>  // send string, receive string
  send(request) → Result<void>                      // fire-and-forget
  write(data, len) → Result<void>                   // raw byte write
  read(buf, max_len, timeout) → Result<size_t>      // raw byte read
  reset()
  abort()
```

`ITransport` is a type alias for `IBufferedTransport` (kept for source compat).

## Architecture

The streaming path is layered:

```
SerialHal / I2CHal        Your hardware (4-5 methods)
  ↓
NotecardSerial            Implements TransportHal (adapts SerialHal)
  ↓
StreamingTransport        Protocol logic: retry, CRC, JSON framing
  ↓
IStreamingTransport       Interface consumed by Notecard
  ↓
Notecard                  Constructed with (IStreamingTransport&, Allocator)
```

`TransportHal` is the boundary between hardware and protocol. It has five
pure virtual methods: `transmit()`, `read()`, `reset()`,
`write_line_terminator()`, `delay()`. `StreamingTransport` owns all
protocol logic — retry loops, CRC (append on send, verify on receive), and
JSON request/response framing. Zero internal buffers.

## Usage

### Arduino — streaming just works

```cpp
#include <note/arduino.hpp>

note::arduino::Notecard nc;
nc.begin(Serial1, 9600);

auto result = nc.card.version().execute();
if (result) {
    auto version = result.version;
}
```

`begin()` configures a default heap allocator automatically. Requests
stream directly to the transport (no intermediate buffer). Responses are
SAX-parsed directly from the transport byte stream — fields populate as
bytes arrive, strings are interned into heap-backed storage.

No buffers. No tree. No copies.

### Low memory — explicit arena, zero heap

On constrained platforms (or when you want zero heap allocation), provide
a `MonotonicArena`:

```cpp
#include <note/arduino.hpp>
#include <note/allocator.hpp>

char pool[512];
note::MonotonicArena arena(pool);

note::arduino::Notecard nc;
nc.begin(Serial1, 9600, arena_allocator(arena));

auto result = nc.card.version().execute();
// String fields in result are backed by the arena, not the heap.
// Call arena.reset() between request cycles to reclaim memory.
```

The arena replaces the heap allocator. All string interning during
response parsing goes into the arena buffer. `arena.reset()` reclaims
everything — call it between request/response cycles.

For I2C:
```cpp
nc.begin(Wire, arena_allocator(arena));
```

### Non-Arduino — streaming path (recommended)

Wire up the three layers directly: `SerialHal` -> `NotecardSerial` (`TransportHal`) -> `StreamingTransport` (`IStreamingTransport`) -> `Notecard`:

```cpp
#include <note/streaming_transport.hpp>
#include <note/transport/serial.hpp>
#include <note/arena.hpp>

MySerialHal hal;                                    // your SerialHal impl
note::transport::NotecardSerial serial_hal(hal);    // TransportHal
note::StreamingTransport transport(serial_hal);     // IStreamingTransport

char pool[256];
note::MonotonicArena arena(pool);
note::Notecard nc(transport, note::arena_allocator(arena));  // zero heap
```

No `JsonBackend` required. No `std::string` linked. No `operator new`.

### Non-Arduino — buffered path (tests/compat)

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::transport::NotecardI2c transport(hal);        // IBufferedTransport
note::Notecard nc(backend, transport);
nc.set_allocator(note::Allocator{});                // optional: arena or heap
```

## Notecard Constructors

| Constructor | Transport | Backend | Heap |
|---|---|---|---|
| `Notecard(IStreamingTransport&, Allocator)` | Streaming | None needed | Zero (arena) |
| `Notecard(IBufferedTransport&, JsonBackend&)` | Buffered | Required | Depends on backend |

The streaming-only constructor is the recommended path for production.
It requires no `JsonBackend` — requests build directly into the transport,
responses SAX-parse directly from the wire. The allocator provides backing
storage for string interning (typically a `MonotonicArena`).

The buffered constructor exists for test harnesses (where `CallbackTransport`
+ `MockBackend` is convenient) and for I2C (where `NotecardI2c` still extends
`AbstractTransport`).

## How execute() Selects the Path

`execute()` picks the best path based on what's configured:

| Constructor used | Allocator set | Path |
|---|---|---|
| `IStreamingTransport` | Yes (required) | **Full streaming** — SAX parse from transport, zero buffer |
| `IBufferedTransport` | Optional | **Fully buffered** — `transact()` with string buffer |

The full streaming path:
1. `StreamingJsonBuilder` writes request bytes directly to `TransportHal::transmit()`
2. `sax_parse_streaming()` reads response bytes from `TransportHal::read()`
3. `CrcAccumulator` verifies CRC via a 23-byte delayed ring buffer
4. `Response::Sink` stores fields as SAX callbacks fire
5. `StringPool` interns string values immediately during callbacks

## Execution Paths (Internal)

### Path 1: Full streaming (via `IStreamingTransport`)

Active when `Notecard` was constructed with `(IStreamingTransport&, Allocator)`.

- **Send:** `StreamingJsonBuilder` → `CrcWriter` → `TransportHal::transmit()`
- **Receive:** `TransportHal::read()` → `CrcAccumulator` → `sax_parse_streaming()` → sink chain
- **Sink chain:** `CrcFieldSink` → `ErrorCaptureSink` → `Response::Sink(pool)`
- **CRC:** accumulated incrementally on both send and receive, verified by `StreamingTransport`
- **Strings:** interned into `StringPool` during SAX callbacks
- **Retries:** `sink.reset()` + rebuild request from the (still-alive) request object
- **Buffers:** zero — all data flows through the HAL in small chunks

### Path 2: Fully buffered (via `IBufferedTransport`)

Active when `Notecard` was constructed with `(IBufferedTransport&, JsonBackend&)`.

- **Send:** `BufferJsonBuilder` → `prepare_wire()` → `do_transmit()`
- **Receive:** `do_receive()` into `response_buf_`
- **Parse:** `backend_->get_reader()` → `Rsp::parse(reader)`
- Used by `CallbackTransport` for testing and `NotecardI2c` for I2C

### Binary path (`write` / `read` + COBS)

```cpp
api.card.binary.put().data(buf, len).execute();
```

- `do_binary_send`: COBS encode → stream via `IStreamingTransport::write()` → `TransportHal::transmit()`, MD5 verify
- `do_binary_receive`: stream via `IStreamingTransport::read()` → `TransportHal::read()` → `CobsDecoder.feed()`, MD5 verify
- 64-byte stack chunk — no intermediate buffer for the COBS stream

## CRC Handling

In the streaming path, CRC is handled entirely by `StreamingTransport`:

- **Send:** a `CrcWriter` wraps the `JsonWriter` that writes to `TransportHal::transmit()`. It accumulates the CRC incrementally as JSON bytes are written, then appends the `,"crc":"SSSS:CCCCCCCC"}` suffix.
- **Receive:** a `CrcAccumulator` feeds on bytes as they arrive from `TransportHal::read()`. A `CrcFieldSink` in the SAX sink chain extracts the CRC field value during parsing. After parsing, `StreamingTransport` compares the accumulated checksum against the extracted field.

Auto-detection: `crc_enabled` flips to `true` when the first valid CRC
field is found in a response. All subsequent responses must have CRC.

In the buffered path, CRC uses the in-place buffer functions (`crc_add`,
`crc_check_and_strip`) as before.

## `NOTE_NO_STD_STRING` Guard

The streaming path (`TransportHal` + `StreamingTransport` + `IStreamingTransport`)
has no dependency on `<string>` or `<functional>`. It compiles cleanly with
`NOTE_NO_STD_STRING` defined.

The buffered path's `AbstractTransport` uses `std::string` for wire and
response buffers, guarded behind `#ifndef NOTE_NO_STD_STRING`:

```cpp
#ifndef NOTE_NO_STD_STRING
    std::string wire_;
    std::string response_buf_;
    virtual Result<void> do_receive(std::string& buf, uint32_t timeout_ms) = 0;
#endif
```

On AVR and other platforms without `<string>`, define `NOTE_NO_STD_STRING`
and use the streaming path exclusively. The AVR binary fits in 32 KB:
28,760 bytes (89%) flash with zero heap — no `operator new` linked.

## Remaining Work

### I2C migration to TransportHal

`NotecardI2c` still extends `AbstractTransport` (the buffered path). Migrating
it to implement `TransportHal` would allow I2C to use the streaming path and
benefit from zero-buffer operation.

### Implicit Arena -> Allocator conversion

Currently users must call `arena_allocator(arena)` explicitly. A future
enhancement would add `MonotonicArena::operator Allocator()` for implicit
conversion, allowing `nc.begin(Serial1, 9600, arena)` directly. This
requires restructuring the `allocator.hpp` / `arena.hpp` include chain.

## Resync After Errors

If a binary transfer fails mid-stream, bytes may remain in the transport.
`reset()` drains stale bytes — both I2C and serial implementations send
`\n` and wait for only control characters, matching note-c behavior.
`do_binary_receive()` calls `reset()` on failure before returning.

## Body Content Tiers

Response body content may be freeform JSON. Three tiers, allocation
always under developer control:

1. **SAX stream** — `JsonSink` callbacks, zero allocation
2. **`bodyAs<T>()`** — struct population via `NOTE_FIELDS`, zero intermediate allocation
3. **JSON tree** — explicit `bodyAsJson()`, developer pays for allocation
