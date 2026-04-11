# Memory Management

`note-cpp` can operate with **zero heap allocations** in steady state. This guide explains the memory model, when strings need interning, and how to choose the right configuration.

## Response String Lifetime

Response fields like `r.version` and `r.device` are `string_view`s — they point into memory managed by the allocator or (on the buffered path) the transport buffer, not into the `Response` struct itself.

**Streaming path (with allocator):** string_views are interned into the `MonotonicArena` during SAX parsing. They are valid until `arena.reset()`. This is the default for `Notecard(IStreamingTransport&, Allocator)`.

**Buffered path (without allocator):** string_views point into the transport buffer and are valid until the next `execute()` call. This is fine when you consume response fields immediately:

```cpp
auto r = api.card.version().execute();
std::printf("version: %.*s\n", (int)r.version.size(), r.version.data());
// r.version is valid here — no second execute() has happened yet
```

**Buffered path (with allocator):** string_views are copied into a `MonotonicArena` and survive transport buffer reuse. Use this when response data must outlive the next request:

```cpp
// Store the allocator once — all execute() calls use it automatically
char arena_buf[1024];
note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
nc.set_allocator(note::arena_allocator(arena));

auto r1 = api.card.version().execute();
auto r2 = api.hub.status().execute();
// r1.version is still valid — it lives in the arena, not the transport buffer
```

### When Do You Need an Allocator?

On the streaming path, an allocator is always required (passed to the `Notecard` constructor). On the buffered path, it is optional.

| Scenario | Allocator needed? |
|----------|-------------------|
| Streaming path (any usage) | **Yes** (required by constructor) |
| Buffered: read response, act on it, then next request | No |
| Buffered: store response fields for later comparison | **Yes** |
| Buffered: collect data from multiple requests before processing | **Yes** |
| Fire-and-forget commands (no response) | No |
| Error messages from `r.error().message` with allocator set | Auto-interned |

**Rule of thumb:** on the buffered path, if a response `string_view` must outlive the next `execute()` call, set an allocator. On the streaming path, strings live in the arena until `arena.reset()`.

## Backend Memory Profiles

The streaming path (`Notecard(IStreamingTransport&, Allocator)`) does not use a `JsonBackend` — requests build directly into the transport, responses SAX-parse from the wire. This is the lowest-memory option.

For the buffered path, backend choice determines allocation behavior:

| Backend | JSON building | JSON parsing | Heap allocs (steady state) |
|---------|--------------|-------------|---------------------------|
| *Streaming (no backend)* | *StreamingJsonBuilder* | *SAX parse* | **0** |
| `BufferJsonBackend<N,T>` | Fixed `char[N]` buffer | Fixed `jsmn_tok[T]` array | **0** |
| `CjsonArenaBackend` | cJSON (arena-backed) | cJSON (arena-backed) | **0** (arena) |
| `CjsonBackend` | cJSON (heap) | cJSON (heap) | ~5-10 per request |
| `NlohmannBackend` | nlohmann (heap) | nlohmann (heap) | Many per request |

For zero-allocation operation, use the streaming path or `BufferJsonBackend`.

## Configuration

### Streaming path — zero heap (recommended)

```cpp
note::transport::NotecardSerial serial_hal(hal);    // TransportHal
note::StreamingTransport transport(serial_hal);     // IStreamingTransport

char arena_buf[256];
note::MonotonicArena arena(arena_buf);
note::Notecard nc(transport, note::arena_allocator(arena));
note::Api api(nc);

auto r = api.card.version().execute();  // 0 heap allocs, strings in arena
// r.version valid until arena.reset()
```

No `JsonBackend` needed. No `std::string` linked. No `operator new`.

### Buffered path — with backend

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);  // IBufferedTransport
note::Api api(nc);

auto r = api.card.version().execute();  // 0 heap allocs (BufferJsonBackend)
// Use r.version before next execute()
```

### Buffered path — with StringPool

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);

char arena_buf[1024];
note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
nc.set_allocator(note::arena_allocator(arena));

note::Api api(nc);
auto r = api.card.version().execute();  // 0 heap allocs, strings in arena
// r.version valid until arena.reset()
```

### Arena Reset

The arena grows monotonically — call `reset()` when you're done with all responses from the current batch:

```cpp
// Process a batch of requests
auto r1 = api.card.version().execute();
auto r2 = api.hub.status().execute();
process(r1, r2);

// Done with r1 and r2 — reclaim arena memory
arena.reset();
nc.set_allocator(note::arena_allocator(arena));  // re-bind after reset
```

After `reset()`, all previous `string_view`s from interned responses are invalid.

### Per-Call Allocator

For one-off use without configuring a stored allocator:

```cpp
note::MonotonicArena arena(buf, sizeof(buf));
auto r = nc.execute(req, note::arena_allocator(arena));
```

### Arena Sizing

A `card.version` response typically uses ~46 bytes of arena for string interning. For most Notecard responses, 512-1024 bytes is sufficient. The arena reports usage via `arena.used()` and `arena.capacity()`.

## Allocation Profiling

The integration tests in `tests/integration/buffer/` verify allocation counts:

- **`test_alloc_profile`** — proves `BufferJsonBackend` + tree-parse = 0 heap allocs
- **`test_sax_alloc_profile`** — proves StringPool + arena = 0 heap allocs, strings survive reuse

Both use global `operator new`/`operator delete` overrides to count every C++ heap allocation.

## Binary Transfer Buffers

Binary transfer (`.data()` / `.into()`) follows the same caller-owns-memory
model as the rest of the library.

**PUT** — the caller supplies the source data buffer. `CobsEncoder` is streaming:
it reads from the source and flushes encoded blocks directly to the transport
callback. No scratch buffer needed; the working block buffer defaults to stack
but can be caller-provided for stack-constrained targets.

**GET** — the caller supplies the destination buffer. COBS decoding writes
decoded bytes into it in place as they arrive from the transport.

```cpp
// Stack-allocate the destination buffer; no heap involvement:
uint8_t buf[1024];
auto rsp = api.binary.get(buf, sizeof(buf)).execute();
// rsp.buffer → span<const uint8_t> into buf, sized to decoded bytes received
```

On stack-constrained targets, register a static buffer on the `Notecard` once
at startup. All binary operations then use it automatically:

```cpp
static uint8_t cobs_buf[NOTE_COBS_BLOCK_SIZE];
nc.set_cobs_buffer(cobs_buf, sizeof(cobs_buf));   // or pass a span
// execute() calls need no changes
```

Use `note::cobs_encoded_size(n)` to check Notecard capacity upfront:

```cpp
constexpr size_t raw_len = 1024;
static_assert(note::cobs_encoded_size(raw_len) <= MAX_NOTECARD_BINARY);
```

## Streaming SAX Parser

The streaming path uses `sax_parse_streaming()` internally — called by
`StreamingTransport::receive_streaming()` to parse JSON incrementally
from `TransportHal::read()`. No full-response buffer needed.

```cpp
char buf[384];  // or any size — stack, static, arena
note::SaxStreamBuf sbuf(buf);
auto err = note::sax_parse_streaming(read_fn, timeout_ms, sbuf, sink);
```

The buffer is partitioned into three regions: read buffer (1/6), key scratch
(1/6), and value scratch (4/6). Default overload uses 384 bytes on the stack.
Zero heap allocation — all memory is caller-provided or stack.

See [streaming-transport.md](internal/streaming-transport.md) for the full design and
`include/note/json_sax_streaming.hpp` for the implementation.

## See Also

- [examples/zero_alloc.cpp](../examples/zero_alloc.cpp) — working example of all three patterns
- [docs/transport.md](transport.md) — transport layer architecture (`TransportHal`, `StreamingTransport`)
- [docs/streaming-transport.md](internal/streaming-transport.md) — streaming execution paths and CRC
- [docs/json-backend.md](json-backend.md) — backend selection and customization
- `include/note/transport_hal.hpp` — `TransportHal` interface
- `include/note/streaming_transport.hpp` — `IStreamingTransport`, `StreamingTransport`
- `include/note/arena.hpp` — `MonotonicArena` implementation
- `include/note/allocator.hpp` — `Allocator` type and adapters
- `include/note/string_pool.hpp` — `StringPool` implementation
- `include/note/transport/cobs.hpp` — `CobsEncoder`, `CobsDecoder`, `cobs_encoded_size()`
- [docs/binary-transfer.md](binary-transfer.md) — binary transfer memory model
