# Streaming Transport

The transport layer exposes a single session interface — `ITransport`,
implemented natively by `Protocol` (streaming, zero-buffer) and via the
transitional `IBufferedTransport` bridge for buffered/test transports.
The streaming path is the primary design; the buffered path exists for
test harnesses and for transports that don't naturally split send/read.

## Transport Interfaces

### Protocol (primary)

`Protocol : public ITransport` exposes:

```
ITransport
  transact(req, span<char>, timeout) → Result<string_view>  // string request, copy response into buf
  transact(req, JsonSink&, timeout)  → Result<void>         // string request, SAX-parse response
  send(req)                          → Result<void>         // fire-and-forget string

  transact(RequestSource, span<char>, timeout) → Result<string_view>
  transact(RequestSource, JsonSink&, timeout)  → Result<void>
  send(RequestSource)                          → Result<void>

  write(data, len)             → Result<void>     // raw byte write (binary/COBS)
  read(buf, max_len, timeout)  → Result<size_t>   // raw byte read (binary/COBS)
  reset()                                         // drain + resync
  abort()
```

Plus BuildFn-shaped non-virtual entry points (`transact(BuildFn, ctx, sink, timeout)`,
`send(BuildFn, ctx)`, with templated `F&&` convenience wrappers) on `Protocol`
itself, for callers like `StaticNotecard` and tests that want zero-vtable
direct dispatch.

Requests are streamed via a `RequestSource` (or BuildFn callable) that paints
JSON bytes through a `StreamingJsonBuilder` over the wire. Responses are
SAX-parsed directly from the wire into a `JsonSink&`. No intermediate buffers.

### IBufferedTransport (transitional bridge)

```
IBufferedTransport : public ITransport
  transact(request, timeout) → Result<string_view>  // legacy buffered virtual
  send(request)              → Result<void>         // fire-and-forget string
```

`IBufferedTransport` is a bridge class: it adapts the old
`transact(req, timeout) -> Result<string_view>` shape (where the response
sits in transport-owned storage) to the unified `ITransport` overloads.
The other `ITransport` overloads (`transact(req, span, t)`, `transact(req, sink, t)`,
plus the `RequestSource` shapes) get default impls that materialise into
a stack scratch buffer and forward to the buffered virtual.

Test transports (`note::test::CallbackTransport`, `MockTransport`,
`ScriptedTransport`) and the note-c bridge all derive from
`IBufferedTransport`. `IBufferedTransport` itself is scheduled to dissolve
into a free helper in step 8c — at which point those subclasses implement
`ITransport` directly.

## Architecture

The streaming path is layered:

```
SerialHal / I2CHal        Your hardware (4-5 methods)
  ↓
NotecardSerial            Implements Hal (adapts SerialHal)
  ↓
Protocol                  Protocol logic: retry, CRC, JSON framing — IS-A ITransport
  ↓
ITransport                Session interface consumed by Notecard
  ↓
Notecard                  Constructed with (Protocol&, Allocator) for streaming-only,
                          or (JsonBackend*, ITransport&, Allocator) for the unified path
```

`Hal` is the boundary between hardware and protocol. It has five
pure virtual methods: `transmit()`, `read()`, `reset()`,
`write_line_terminator()`, `delay()`. `Protocol` owns all
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

Wire up the three layers directly: `SerialHal` -> `NotecardSerial` (`Hal`) -> `Protocol` (`ITransport`) -> `Notecard`:

```cpp
#include <note/protocol.hpp>
#include <note/transport/serial.hpp>
#include <note/arena.hpp>

MySerialHal hal;                                    // your SerialHal impl
note::transport::NotecardSerial serial_hal(hal);    // Hal
note::Protocol transport(serial_hal);     // ITransport

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
| `Notecard(Protocol&, Allocator)` | Streaming | None needed | Zero (arena) |
| `Notecard(JsonBackend&, ITransport&)` | Buffered | Required | Depends on backend |
| `Notecard(JsonBackend*, ITransport&, Allocator)` | Unified | Optional | Depends on backend / arena |

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
| `ITransport` | Yes (required) | **Full streaming** — SAX parse from transport, zero buffer |
| `IBufferedTransport` | Optional | **Fully buffered** — `transact()` with string buffer |

The full streaming path:
1. `StreamingJsonBuilder` writes request bytes directly to `Hal::transmit()`
2. `sax_parse_streaming()` reads response bytes from `Hal::read()`
3. `CrcAccumulator` verifies CRC via a 23-byte delayed ring buffer
4. `Response::Sink` stores fields as SAX callbacks fire
5. `StringPool` interns string values immediately during callbacks

## Execution Paths (Internal)

### Path 1a: JSON streaming (default, via `ITransport`)

Active when `NOTE_JSONB=0` (the default for non-MINIMAL builds).

- **Send:** `StreamingJsonBuilder` → `CrcWriter` → `Hal::transmit()`
- **Receive:** `Hal::read()` → `CrcAccumulator` → `sax_lex_streaming()` → sink chain
- **Sink chain:** `ReceiveContext` (err/crc interception) → `Response::Sink(pool)`
- **CRC:** accumulated incrementally on both send and receive, verified by `Protocol`
- **Wire format:** `{"req":"card.version",...}\r\n`
- **Strings:** interned into `StringPool` during SAX callbacks
- **Retries:** `sink.reset()` + rebuild request from the (still-alive) request object
- **Buffers:** zero — all data flows through the HAL in small chunks

### Path 1b: JSONB streaming (via `ITransport`, `NOTE_JSONB=1`)

Active when `NOTE_JSONB=1` (auto-enabled by `NOTE_MINIMAL`). See [docs/jsonb.md](../jsonb.md).

- **Send:** `StreamingJsonbBuilder` → `CobsStreamWriter` → `Hal::transmit()`. Transport writes `{:` header, builder emits JSONB opcodes through COBS encoder, transport writes `:}` trailer + line terminator.
- **Receive:** `Hal::read()` → `frame_read` (stops at `\n`) → strip `{:` header → `CobsDecodingReader` (COBS decode) → `jsonb_parse_streaming()` → `JsonbParser` → sink chain
- **Sink chain:** Same as JSON path — `ReceiveContext` (err interception) → `Response::Sink(pool)`
- **CRC:** disabled — COBS framing provides integrity
- **Wire format:** `{:<COBS-encoded JSONB opcodes>:}\r\n`
- **Parser depth tracking:** stops at root `kEndObject` to avoid consuming trailer bytes
- **Strings:** JSONB strings are null-terminated in the opcode stream; interned into `StringPool` during SAX callbacks (same as JSON path)
- **Buffers:** `CobsStreamWriter` uses a 255-byte block buffer (same as `CobsEncoder`). `CobsDecodingReader` uses a 64-byte decode buffer. `SaxStreamBuf` uses 128 bytes for the JSONB path (vs 384 for JSON).

**Key components** (all in `include/note/jsonb.hpp`):

| Component | Role |
|-----------|------|
| `StreamingJsonbBuilder` | `JsonBuilder` impl — emits JSONB opcodes to a `JsonWriter` |
| `CobsStreamWriter` | `JsonWriter` impl — COBS-encodes bytes incrementally (255-byte block) |
| `JsonbParser` | Reads opcodes, dispatches `SaxEvent`s through `SaxDispatch` |
| `CobsDecodingReader` | `ReadFn` adapter — COBS-decodes wire bytes, strips `:}` trailer |

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

- `do_binary_send`: COBS encode → stream via `ITransport::write()` → `Hal::transmit()`, MD5 verify
- `do_binary_receive`: stream via `ITransport::read()` → `Hal::read()` → `CobsDecoder.feed()`, MD5 verify
- 64-byte stack chunk — no intermediate buffer for the COBS stream

## CRC Handling

CRC applies to the **JSON path only** (`NOTE_JSONB=0`). When `NOTE_JSONB=1`,
the entire CRC mechanism is bypassed — COBS framing provides its own integrity.

In the JSON streaming path, CRC is handled entirely by `Protocol`:

- **Send:** a `CrcWriter` wraps the `JsonWriter` that writes to `Hal::transmit()`. It accumulates the CRC incrementally as JSON bytes are written, then appends the `,"crc":"SSSS:CCCCCCCC"}` suffix.
- **Receive:** a `CrcAccumulator` feeds on bytes as they arrive from `Hal::read()`. The `ReceiveContext` wrapping dispatch extracts the CRC field value during parsing. After parsing, `Protocol` compares the accumulated checksum against the extracted field.

Auto-detection: `crc_enabled` flips to `true` when the first valid CRC
field is found in a response. All subsequent responses must have CRC.

In the buffered path, CRC uses the in-place buffer functions (`crc_add`,
`crc_check_and_strip`) as before.

## `NOTE_NO_STD_STRING` Guard

The streaming path (`Hal` + `Protocol` + `ITransport`)
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

### I2C migration to Hal

`NotecardI2c` still extends `AbstractTransport` (the buffered path). Migrating
it to implement `Hal` would allow I2C to use the streaming path and
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
2. **`.into(T&)`** — struct population via `NOTE_FIELDS`, zero intermediate allocation
3. **JSON tree** — `result->body()` returns a `JsonReader*`, developer pays for allocation
