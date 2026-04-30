# Streaming Transport

The transport layer exposes a single session interface — `ITransact` —
implemented natively by `Protocol` (streaming, zero-buffer) and by buffered
transports (test fakes, the note-c bridge) that override only the
string_view-shaped virtuals and inherit `ITransact`'s materialise-and-forward
defaults for the `RequestSource` shape. The streaming path is the primary
design; the buffered path exists for test harnesses and for transports that
don't naturally split send/read.

## Transport Interfaces

### Protocol (primary)

`Protocol : public ITransact` exposes:

```
ITransact
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

### Buffered transports

Test transports (`note::test::CallbackTransport`, `MockTransport`,
`ScriptedTransport`) and the note-c bridge derive from `ITransact`
directly and override only the string_view-shaped virtuals. The
`RequestSource` virtuals on `ITransact` carry default impls that
materialise the source into a stack scratch buffer, append the closing
`}`, and forward to the matching string_view virtual — so buffered
transports satisfy the full `ITransact` API without per-class
boilerplate beyond their own `transact(string_view, span<char>, t)`
and `send(string_view)` overrides.

## Architecture

The streaming path is layered. Each layer has one job; bytes flow up
on send and down on receive.

```mermaid
flowchart TD
    hw["SerialHal / I2cHal<br/><i>your hardware (5 methods)</i>"]
    framer["SerialFramer / I2cFramer<br/><i>implements <tt>Hal</tt>; adapts the byte HAL</i>"]
    protocol["Protocol<br/><i>retry, CRC, JSON/JSONB framing — IS-A <tt>ITransact</tt></i>"]
    iface["ITransact<br/><i>session contract consumed by Notecard</i>"]
    notecard["Notecard<br/><i><tt>(Protocol&, Allocator)</tt> streaming-only, or <tt>(JsonBackend*, ITransact&, Allocator)</tt> unified</i>"]
    hw --> framer --> protocol --> iface --> notecard
```

`Hal` is the boundary between hardware and protocol. Five pure virtuals:
`transmit()`, `read()`, `reset()`, `write_line_terminator()`, `delay()`.
`Protocol` owns all protocol logic — retry loops, CRC (append on send,
verify on receive), and JSON/JSONB framing. Zero internal buffers.

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

Wire up the three layers directly: `SerialHal` -> `SerialFramer` (`Hal`) -> `Protocol` (`ITransact`) -> `Notecard`:

```cpp
#include <note/protocol.hpp>
#include <note/link/serial.hpp>
#include <note/arena.hpp>

MySerialHal hal;                                    // your SerialHal impl
note::link::SerialFramer serial_hal(hal);    // Hal
note::Protocol transport(serial_hal);     // ITransact

char pool[256];
note::MonotonicArena arena(pool);
note::Notecard nc(transport, note::arena_allocator(arena));  // zero heap
```

No `JsonBackend` required. No `std::string` linked. No `operator new`.

### Non-Arduino — buffered path (tests/compat)

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::link::I2cFramer transport(hal);        // ITransact
note::Notecard nc(backend, transport);
nc.set_allocator(note::Allocator{});                // optional: arena or heap
```

## Notecard Constructors

| Constructor | Transport | Backend | Heap |
|---|---|---|---|
| `Notecard(Protocol&, Allocator)` | Streaming | None needed | Zero (arena) |
| `Notecard(JsonBackend&, ITransact&)` | Buffered | Required | Depends on backend |
| `Notecard(JsonBackend*, ITransact&, Allocator)` | Unified | Optional | Depends on backend / arena |

The streaming-only constructor is the recommended path for production.
It requires no `JsonBackend` — requests build directly into the transport,
responses SAX-parse directly from the wire. The allocator provides backing
storage for string interning (typically a `MonotonicArena`).

The buffered constructor exists for test harnesses (where `CallbackTransport`
+ `MockBackend` is convenient) and for I2C (where `I2cFramer` still extends
`AbstractTransport`).

## How execute() Selects the Path

`execute()` picks the best path based on what's configured:

| Constructor used | Allocator set | Path |
|---|---|---|
| `ITransact` (Protocol-typed) | Yes (required) | **Full streaming** — SAX parse from transport, zero buffer |
| `ITransact` (buffered subclass) | Optional | **Fully buffered** — `transact()` with caller-supplied buffer |

The full streaming path:
1. `StreamingJsonBuilder` writes request bytes directly to `Hal::transmit()`
2. `sax_parse_streaming()` reads response bytes from `Hal::read()`
3. `CrcAccumulator` verifies CRC via a 23-byte delayed ring buffer
4. `Response::Sink` stores fields as SAX callbacks fire
5. `StringPool` interns string values immediately during callbacks

## Execution Paths (Internal)

### Path 1a: JSON streaming (default, via `ITransact`)

Active when `NOTE_JSONB=0` (the default for non-MINIMAL builds).

- **Send:** `StreamingJsonBuilder` → `CrcWriter` → `Hal::transmit()`
- **Receive:** `Hal::read()` → `CrcAccumulator` → `sax_lex_streaming()` → sink chain
- **Sink chain:** `ReceiveContext` (err/crc interception) → `Response::Sink(pool)`
- **CRC:** accumulated incrementally on both send and receive, verified by `Protocol`
- **Wire format:** `{"req":"card.version",...}\r\n`
- **Strings:** interned into `StringPool` during SAX callbacks
- **Retries:** `sink.reset()` + rebuild request from the (still-alive) request object
- **Buffers:** zero — all data flows through the HAL in small chunks

### Path 1b: JSONB streaming (via `ITransact`, `NOTE_JSONB=1`)

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

### Path 2: Fully buffered (via buffered `ITransact` subclass)

Active when `Notecard` was constructed with `(JsonBackend&, ITransact&)` —
the buffered-only convenience ctor — and the transport overrides only
the string_view-shaped virtuals (e.g. `note::test::CallbackTransport`,
the note-c bridge, integration test fakes).

- **Send:** the configured `JsonBackend`'s builder paints a complete
  request into a caller-supplied `span<char>`; the transport's
  `transact(string_view, span<char>, t)` override sends those bytes
  and returns the response in the same buffer.
- **Receive:** the response `string_view` is fed to
  `backend_->parse_response(...)`, which returns a `JsonReader`; the
  generated `Rsp::parse(reader)` walks fields out of the tree.
- The `RequestSource` virtuals on `ITransact` carry default impls that
  materialise the source into a stack scratch buffer and forward to
  the matching string_view virtual, so buffered transports satisfy
  the full `ITransact` API without per-class boilerplate.

### Binary path (`write` / `read` + COBS)

```cpp
api.card.binary.put().data(buf, len).execute();
```

- `do_binary_send`: COBS encode → stream via `ITransact::write()` → `Hal::transmit()`, MD5 verify
- `do_binary_receive`: stream via `ITransact::read()` → `Hal::read()` → `CobsDecoder.feed()`, MD5 verify
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

The streaming path (`Hal` + `Protocol` + `ITransact`) has no dependency
on `<string>` or `<functional>`. It compiles cleanly with
`NOTE_NO_STD_STRING` defined.

The buffered path runs over caller-supplied `span<char>` buffers — no
`std::string` in the core. Only specific JSON backends bring in
`<string>` (cJSON's wrapper, nlohmann's, etc.) and only when those
backends are linked. AVR builds (which exercise the full streaming
path under `NOTE_MINIMAL`) skip every `<string>`-bearing translation
unit. The current AVR baseline lives in `tools/binary-size-comparison/
avr_baselines.json` (currently ~25 KB / 78% with zero heap).

## Remaining Work

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
