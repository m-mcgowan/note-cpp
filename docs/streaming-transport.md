# Streaming Transport — Design Document

## Status: Proposed

This document captures the agreed design direction for refactoring the transport
layer to support pure streaming and eliminate intermediate buffers.

## Motivation

The current transport layer (`AbstractTransport`) owns a `std::string response_buf_`
that receives the entire JSON response before parsing begins. This has two issues:

1. **Heap allocation** — `std::string` allocates on the heap. Even though the buffer
   is reused across calls (no per-request allocation in steady state), the initial
   allocation is heap-backed. This conflicts with the zero-allocation goal achievable
   via `BufferJsonBackend`.

2. **Unnecessary buffering for binary** — COBS binary data can be streamed through
   the `CobsDecoder` chunk-by-chunk. Buffering the entire COBS stream before
   decoding wastes memory and adds latency.

## Current Architecture

```
ITransport
  transact(request, timeout) → Result<string_view>   // JSON request/response
  send(request) → Result<void>                        // fire-and-forget
  write(data, len) → Result<void>                     // raw byte write (new)
  read(buf, max_len, timeout) → Result<size_t>        // raw byte read (new)
  reset()                                             // drain + resync
  abort()

AbstractTransport : ITransport
  owns: std::string wire_          // outbound request buffer
  owns: std::string response_buf_  // inbound response buffer (the problem)
  transact() = prepare_wire() → do_transmit() → do_receive(response_buf_) → CRC check
  write()    = do_write()     // delegates to HAL, no framing
  read()     = do_read()      // delegates to HAL, blocks for at least 1 byte
```

### JSON path (current)

1. `transact()` clears `response_buf_` and calls `do_receive(response_buf_, timeout)`
2. `do_receive()` appends chunks to the `std::string` until `\n`
3. CRC check on the complete response
4. Returns `string_view` into `response_buf_`
5. `JsonBackend` parses from that `string_view`

### Binary path (new, using `write`/`read`)

1. JSON handshake via `transact()` (card.binary.put/get)
2. COBS streaming via `write()`/`read()` — no intermediate buffer

## Proposed Architecture

### Principle: transport is a byte pipe

The transport should not own response buffers. It provides two primitives:

- **`write(data, len)`** — send raw bytes to the Notecard
- **`read(buf, max_len, timeout)`** — read available bytes from the Notecard

All buffering and parsing happens at higher layers. The caller provides any
buffers needed.

### JSON path (proposed)

The buffer comes from the caller, all the way from the top:

```
caller buffer → Notecard::execute()
  → transport.read(caller_buf, max_len, timeout)  // receive into caller's buffer
  → CRC check on caller_buf
  → JsonBackend parses from caller_buf
  → string_views point into caller_buf
```

With the streaming SAX parser (`JsonSink`), even the full-response buffer
becomes optional — the SAX parser processes tokens as they arrive, and only
string values need a small pool (arena) to survive chunk boundaries.

```
Notecard::execute()
  → transport.read(small_chunk, 64, timeout)    // read a chunk
  → SaxParser.feed(small_chunk)                 // parse incrementally
  → JsonSink.on_string(key, value)              // populate response fields
  → StringPool.intern(value)                    // keep string alive
  → repeat until response complete
```

No intermediate buffer for the full response. Just a 64-byte chunk on the
stack and a string pool for string field values.

### Binary path (proposed, implemented)

Already streaming — `CobsDecoder.feed()` processes chunks as they arrive:

```
Notecard::do_binary_receive()
  → transport.read(chunk, 64, timeout)          // read a chunk
  → CobsDecoder.feed(chunk, callback)           // decode incrementally
  → callback copies decoded bytes to caller's destination buffer
  → repeat until EOP ('\n') seen
```

No intermediate buffer for the COBS stream. Just a stack chunk and the
caller's destination buffer.

### `transact()` as a composed operation

Once `read()`/`write()` are the primitives, `transact()` becomes a composed
operation built on top of them:

```
transact(request, buf, max_len, timeout):
  prepare_wire(request)                          // CRC if enabled
  write(wire_data, wire_len)                     // send request
  n = read_until_eop(buf, max_len, timeout)      // receive response (loop on read())
  crc_check(buf, n)                              // verify CRC
  return string_view(buf, n)                     // caller owns buf
```

The `read_until_eop` helper loops on `read()`, accumulating into the
caller-provided buffer until `\n` is found. This replaces `do_receive()`.

### Resync after errors

If a binary transfer fails mid-stream (e.g. destination buffer too small,
timeout, MD5 mismatch), bytes may remain in the transport. The next JSON
`transact()` would read stale COBS data instead of a JSON response.

The existing `reset()` method handles this — both I2C and serial
implementations send `\n` and drain until only control characters come back,
matching note-c's `_serialNoteReset` / `_i2cNoteReset`. After a failed
binary transfer, `Notecard::do_binary_receive()` should call `reset()` to
flush remaining bytes before returning the error.

## Migration path

### Phase 1 (current): add `write()`/`read()` alongside existing `transact()`

- `write()` and `read()` added to `ITransport` with error defaults
- `AbstractTransport` overrides via `do_write()`/`do_read()` protected virtuals
- I2C and serial implement `do_write()`/`do_read()` using their HALs
- Binary path uses `write()`/`read()` for streaming COBS
- JSON path continues using `transact()` with `response_buf_` (unchanged)
- `CallbackTransport` accepts optional write/read callbacks for testing

### Phase 2: caller-provided buffers for `transact()`

- Add `transact(request, buf, max_len, timeout)` overload
- Receives into caller's buffer instead of `response_buf_`
- `response_buf_` becomes optional / deprecated
- Enables zero-allocation through the full stack

### Phase 3: streaming JSON via SAX

- Build `transact_streaming(request, sink, timeout)` using `read()` + SAX parser
- Response fields populated incrementally via `JsonSink` callbacks
- Only string values buffered (via `StringPool` / arena)
- `response_buf_` eliminated entirely
- Full zero-allocation: no heap in transport, backend, or response parsing

## Impact on `do_receive()` / `do_transmit()`

Once `transact()` is rebuilt on `read()`/`write()`:

- `do_receive(std::string&, timeout)` — no longer needed
- `do_transmit(const char*, size_t)` — replaced by `do_write(uint8_t*, size_t)`
- `prepare_wire()` — moves to `transact()` implementation, not a virtual

The `AbstractTransport` building blocks simplify to just `do_write()`,
`do_read()`, and `do_reset()`.

## Testing: Transport Fuzzing

Integration tests should include transport fuzzing to exercise error recovery
on both send and receive paths:

- **Write errors**: HAL transmit fails mid-stream (partial COBS send). Verify
  `reset()` is called and the next JSON transaction succeeds.
- **Read errors**: HAL returns partial data, then times out. Verify `reset()`
  drains stale bytes and resynchronizes.
- **Truncated response**: Read buffer too small for COBS data. Verify the
  transport resyncs (no stale bytes corrupt the next transaction).
- **Corrupted data**: Random byte flips in COBS stream. Verify MD5 mismatch
  is detected and reported.
- **Interleaved operations**: Binary transfer followed immediately by JSON
  request. Verify no cross-contamination.

The `CallbackTransport` with `set_write()`/`set_read()` enables unit-level
fuzzing. Integration tests on real hardware should use `serial-monitor` capture
to verify wire-level behavior matches expectations.

## Backend dissolution

With full streaming (Phase 3), the `JsonBackend` abstraction dissolves.
There is no object that "owns" JSON parsing or building — JSON is just the
wire format the Notecard chose. The architecture becomes:

```
Developer code
  ↕  typed fields (C++ structs, ResponseField<T>)
SAX stream
  ↕  events (on_string, on_number, on_bool, begin_object, ...)
Transport byte pipe
  ↕  raw bytes (read/write)
Notecard hardware
```

No intermediate buffer, no backend choice, no allocator configuration.
The developer works with typed fields. The library handles serialization
as a transport detail.

### Implication for `NotecardApi` / `arduino::Notecard`

Today these bundle a `BufferJsonBackend` as the default. With streaming,
they bundle a SAX parser + small chunk buffer instead. The API surface
doesn't change — `execute()` still returns typed `ApiResult<Response>`.
The backend parameter disappears from constructors entirely.

## Body content layering

Response body content (`note.get`, inbound notes) may be freeform JSON
whose schema isn't known at compile time. Streaming provides three tiers
for handling body content, with allocation always under the developer's
explicit control:

### Tier 1: SAX stream (zero allocation)

The developer provides a `JsonSink` that handles events directly. No
intermediate representation. Useful for filtering, forwarding, or
populating application state incrementally.

```cpp
struct MySink : note::JsonSink {
    void on_string(string_view key, string_view value) override {
        if (key == "target") display.show(value);
    }
};
```

### Tier 2: `bodyAs<T>()` (struct population, zero intermediate allocation)

The SAX stream feeds directly into a C++ struct via `NOTE_FIELDS` or
C++20 aggregate reflection. No JSON tree built — fields are assigned
as the tokens arrive.

```cpp
auto r = nc.note.get().file("config.qi").execute();
auto config = r.bodyAs<MyConfig>();
```

### Tier 3: JSON tree (explicit allocation)

A convenience `JsonTreeSink` builds an in-memory JSON object from
the SAX stream. The developer explicitly opts into this allocation.
Useful for freeform content that needs inspection, iteration over
unknown keys, or forwarding to another JSON consumer.

```cpp
auto r = nc.note.get().file("data.qi").execute();
auto tree = r.bodyAsJson();  // developer pays for allocation here
for (auto& [key, value] : tree) {
    // iterate over unknown fields
}
```

### Design principle

The library never allocates on behalf of the developer without their
knowledge. Tier 1 and 2 are zero-allocation. Tier 3 allocates because
the developer asked for a tree — the cost is visible in their code,
not hidden inside a backend.

This means:
- No backend configuration knob for users
- No hidden buffer sizes to tune
- Memory usage is proportional to what the developer explicitly requests
- The "which backend?" question disappears entirely
