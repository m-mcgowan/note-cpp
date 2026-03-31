# Streaming Transport

The transport layer provides three request execution paths with different
allocation trade-offs. All three share the same `ITransport` interface.

## Transport Interface

```
ITransport
  transact(request, timeout) → Result<string_view>   // JSON request/response
  send(request) → Result<void>                        // fire-and-forget
  write(data, len) → Result<void>                     // raw byte write
  read(buf, max_len, timeout) → Result<size_t>        // raw byte read
  reset()                                             // drain + resync
  abort()
```

`write()` and `read()` are byte-stream primitives — no framing, CRC, or
line terminators. Used for binary (COBS) streaming.

`transact()` and `send()` handle the full JSON protocol: wire buffer
preparation, CRC, retry, and response parsing.

## Usage

### Default — streaming just works

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

### Low Memory — explicit arena control

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

### Non-Arduino — direct transport setup

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::transport::NotecardSerial transport(hal);
note::Notecard nc(backend, transport);  // AbstractTransport& → streaming
nc.set_allocator(note::Allocator{});    // heap, or arena_allocator(arena)
```

## How execute() Selects the Path

`execute()` picks the best path based on what's configured:

| Streaming transport | Allocator set | Path |
|---|---|---|
| Yes | Yes | **Full streaming** — SAX parse from transport, zero buffer |
| Yes | No | **Streaming send** + buffered receive |
| No | — | **Fully buffered** — legacy `transact()` path |

The full streaming path:
1. `StreamingJsonBuilder` writes request bytes directly to `do_write()`
2. `sax_parse_streaming()` reads response bytes from `do_read()`
3. `CrcAccumulator` verifies CRC via a 23-byte delayed ring buffer
4. `Response::Sink` stores fields as SAX callbacks fire
5. `StringPool` interns string values immediately during callbacks

## Execution Paths (Internal)

### Path 1: Full streaming (`transact_streaming`)

Active when `streaming_transport_` and `alloc_` are both set.

- **Send:** `StreamingJsonBuilder` → `CrcWriter` → `RawWriter` → `do_write()`
- **Receive:** `do_read()` → `CrcAccumulator` → `sax_parse_streaming()` → sink chain
- **Sink chain:** `CrcFieldSink` → `ErrorCaptureSink` → `Response::Sink(pool)`
- **CRC:** accumulated incrementally on both send and receive
- **Strings:** interned into `StringPool` during SAX callbacks
- **Retries:** `sink.reset()` + rebuild request from the (still-alive) request object

### Path 2: Streaming send + buffered receive (`transact_build`)

Active when `streaming_transport_` is set but no allocator.

- **Send:** streaming (same as Path 1)
- **Receive:** response buffered into `ext_buf_` or `response_buf_`
- **Parse:** `backend_->get_reader()` → `Rsp::parse(reader)`
- Falls back here when response types lack a `Sink` (e.g. test harness types)

### Path 3: Fully buffered (`transact`)

Active when using `ITransport` (e.g. `CallbackTransport` for testing).

- **Send:** `BufferJsonBuilder` → `prepare_wire()` → `do_transmit()`
- **Receive:** `do_receive()` into `response_buf_`
- Legacy path — requires `<string>`

### Path 4: Caller-provided buffers (`set_receive_buffer` / `transact_into`)

```cpp
char buf[512];
transport.set_receive_buffer(buf, sizeof(buf));
auto rsp = nc.card.version().execute();
```

- Zero heap for the receive path
- `response_buf_` bypassed
- CRC check in-place on the caller buffer

### Binary path (`write` / `read` + COBS)

```cpp
api.card.binary.put().data(buf, len).execute();
```

- `do_binary_send`: COBS encode → stream via `write()`, MD5 verify
- `do_binary_receive`: stream via `read()` → `CobsDecoder.feed()`, MD5 verify
- 64-byte stack chunk — no intermediate buffer for the COBS stream

## CRC Handling

CRC operates on char buffers — no `std::string` involved:

```cpp
// Append CRC to outbound request (in-place)
wire_len_ = crc_add(wire_data(), wire_len_, wire_capacity(), crc_seq_);

// Verify and strip CRC from inbound response (in-place)
crc_check_and_strip(buf, len, expected_seq, crc_enabled);
```

Auto-detection: `crc_enabled` flips to `true` when the first valid CRC
field is found in a response. All subsequent responses must have CRC.

## `NOTE_NO_STD_STRING` Guard

Code that depends on `<string>` or `<functional>` is guarded:

```cpp
#ifndef NOTE_NO_STD_STRING
    std::string wire_;
    std::string response_buf_;
    virtual Result<void> do_receive(std::string& buf, uint32_t timeout_ms) = 0;
    // ... legacy transact, streaming transact, binary pipeline ...
#endif
```

When `NOTE_NO_STD_STRING` is defined, only Path 2 (`transact_into` /
`set_receive_buffer`) is available. This enables compilation on platforms
without `<string>` (e.g. AVR with polyfills from the compat project).

## Remaining Work

### Opt-in / opt-out mechanism

Once streaming is verified reliable on all platforms, it should be the
default. The transition mechanism is deferred — options include a
`#define NOTE_STREAMING`, per-Notecard configuration, or simply removing
the buffered fallback.

### `wire_` std::string removal

The legacy `transact()` path still uses `std::string wire_` for the send
buffer. The streaming send path (`transact_build`) bypasses it entirely.
Once all callers migrate to streaming, `wire_` can be removed.

### Implicit Arena → Allocator conversion

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
