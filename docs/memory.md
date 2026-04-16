# Memory Management

`note-cpp` can run with **zero heap allocations** in steady state — important on embedded targets without a heap, or where you just don't want to pay for `malloc` on every request.

This guide covers two questions that come up as soon as you go past "hello world":

1. **"How long are response strings valid?"** — fields like `r.version` are views, not copies; they become invalid at some point.
2. **"What memory knobs do I have if I'm flash/RAM-constrained?"** — backends, arenas, streaming vs. buffered.

The short version for both: **by default things work**. You only need this guide when the default isn't enough.

## Do I need to do anything special?

| Scenario | What to do |
|----------|------------|
| Read a response, use it immediately, call the next request | **Nothing.** Default works. |
| Keep response fields around past the next `execute()` | Set an **arena allocator** (below). |
| Collect data from several requests before processing | Arena allocator. |
| Fire-and-forget commands (no response) | Nothing. |
| Using the streaming path | Arena is **required** by the constructor — the library won't compile without one. |
| Running on a target with no heap / strict flash budget | Use the streaming path, or `BufferJsonBackend`. See [Backend profiles](#backend-memory-profiles). |

If you're in the first row, stop reading. If you're anywhere else, continue.

## Response string lifetimes

Response fields like `r.version`, `r.device`, and most string fields are `std::string_view` — pointers into memory the library owns, not copies. They're cheap, but they have an expiry date.

Three rules:

1. **Default (buffered, no arena):** views are valid **until the next `execute()` call**. This is the common case — read, use, move on.
2. **With an arena allocator:** views are copied into the arena and are valid **until you call `arena.reset()`**.
3. **Streaming path (always uses an arena):** same as above — views are valid until the arena is reset.

The arena is the "keep this string around" mechanism. Without it, response memory is recycled on the next request.

### Setting an arena

```cpp
char arena_buf[1024];
note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
nc.set_allocator(note::arena_allocator(arena));

auto r1 = api.card.version().execute();
auto r2 = api.hub.status().execute();
// r1.version is still valid — it was copied into the arena,
// not left in the transport buffer.
```

"Arena" is a common term for a bump allocator — you hand it a block of memory, it gives out pieces linearly, and you free everything at once by calling `reset()`. No per-allocation bookkeeping, no fragmentation. When a response has strings to keep alive, `note-cpp` copies them into the arena (this copy is the "intern" step).

When you're done with a batch of responses, reclaim the memory:

```cpp
// Finished with r1 and r2
arena.reset();
nc.set_allocator(note::arena_allocator(arena));  // re-bind after reset
```

After `reset()`, every `string_view` that pointed into the arena is invalid. Don't touch them.

### Sizing the arena

A `card.version` response uses ~46 bytes of arena. A `hub.status` response with a connected message string might use ~120. For most apps, **512–1024 bytes is plenty**. You can check with `arena.used()` and `arena.capacity()`.

If you want to know the worst-case arena size at compile time, each generated request has `Request::Response::max_arena_size` (see [arena-sizing.md](arena-sizing.md)).

### Per-call arena (no set_allocator)

If you only need an arena for a specific call:

```cpp
note::MonotonicArena arena(buf, sizeof(buf));
auto r = nc.execute(req, note::arena_allocator(arena));
```

## Choosing a path: streaming vs. buffered

`note-cpp` has two execution paths. Pick one based on your transport and memory constraints:

- **Streaming path** — builds the request directly into the transport and parses the response with a SAX parser as bytes arrive. No request/response buffers needed. **Always zero heap.** Requires an arena (passed to the constructor).
- **Buffered path** — builds the full request in a `JsonBackend`, sends it, reads the full response back, then parses. Simpler mental model. Heap allocation depends on the backend.

See [transport.md](transport.md#streaming-vs-buffered) for a side-by-side comparison and when to choose each.

## Backend memory profiles

On the buffered path, your backend choice decides whether you allocate heap memory per request:

| Backend | JSON build | JSON parse | Heap allocs per request |
|---------|------------|------------|-------------------------|
| *Streaming (no backend)* | direct-to-transport | SAX | **0** |
| `BufferJsonBackend<N,T>` | fixed `char[N]` | fixed token array | **0** |
| `CjsonArenaBackend` | cJSON + arena | cJSON + arena | **0** (all from arena) |
| `CjsonBackend` | cJSON (heap) | cJSON (heap) | ~5–10 |
| `NlohmannBackend` | nlohmann (heap) | nlohmann (heap) | many |

For zero-alloc: streaming path, or `BufferJsonBackend`, or `CjsonArenaBackend`.

## Configuration recipes

### Streaming path — zero heap (recommended for embedded)

```cpp
note::transport::NotecardSerial serial_hal(hal);    // TransportHal
note::StreamingTransport transport(serial_hal);     // IStreamingTransport

char arena_buf[256];
note::MonotonicArena arena(arena_buf);
note::Notecard nc(transport, note::arena_allocator(arena));
note::Api api(nc);

auto r = api.card.version().execute();   // 0 heap allocs, strings in arena
// r.version valid until arena.reset()
```

No backend needed. No `std::string` linked. No `operator new`.

### Buffered path — with `BufferJsonBackend` (zero heap)

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);   // IBufferedTransport
note::Api api(nc);

auto r = api.card.version().execute();   // 0 heap allocs
// Consume r before the next execute()
```

The two template parameters are: request/response buffer size, and jsmn token count. Pick values that fit your largest request and largest response.

### Buffered path — with arena

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);

char arena_buf[1024];
note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
nc.set_allocator(note::arena_allocator(arena));

note::Api api(nc);
auto r = api.card.version().execute();   // 0 heap allocs, strings in arena
// r.version valid until arena.reset()
```

## Advanced: binary transfer buffers

Binary transfer (`.data()` / `.into()`) follows the same caller-owns-memory model as the rest of the library. COBS encoding streams from the source buffer directly to the transport; decoding writes into the destination buffer in place. No scratch buffer required.

```cpp
uint8_t buf[1024];
auto rsp = api.card.binary.get(buf, sizeof(buf)).execute();
// rsp.buffer is a span into buf, sized to the decoded payload length.
```

For stack-constrained targets, register a COBS working buffer once at startup so every binary call uses it:

```cpp
static uint8_t cobs_buf[NOTE_COBS_BLOCK_SIZE];
nc.set_cobs_buffer(cobs_buf, sizeof(cobs_buf));
```

Use `note::cobs_encoded_size(n)` to check capacity at compile time:

```cpp
constexpr size_t raw_len = 1024;
static_assert(note::cobs_encoded_size(raw_len) <= MAX_NOTECARD_BINARY);
```

## Advanced: streaming SAX parser internals

The streaming path parses JSON incrementally via `sax_parse_streaming()`. The parser takes a caller-provided buffer and partitions it into read, key-scratch, and value-scratch regions:

```cpp
char buf[384];
note::SaxStreamBuf sbuf(buf);
auto err = note::sax_parse_streaming(read_fn, timeout_ms, sbuf, sink);
```

Default overloads use 384 bytes on the stack. All memory is caller-provided or stack — no heap. See [streaming-transport.md](internal/streaming-transport.md) for the full design.

## Allocation proof

The integration tests override global `operator new`/`operator delete` to count every C++ heap allocation:

- `tests/integration/buffer/test_alloc_profile` — proves `BufferJsonBackend` + tree parse = 0 heap allocs.
- `tests/integration/buffer/test_sax_alloc_profile` — proves streaming + arena = 0 heap allocs and strings survive transport reuse.

## See also

- [examples/zero-alloc.cpp](../examples/stdcpp/zero-alloc.cpp) — working example of the patterns above
- [response-lifetimes.md](response-lifetimes.md) — in-depth guide to `string_view` validity
- [arena-sizing.md](arena-sizing.md) — computing arena size at compile time
- [transport.md](transport.md#streaming-vs-buffered) — which path to pick
- [json-backend.md](json-backend.md) — backend selection and customization
- [binary-transfer.md](binary-transfer.md) — binary transfer memory model
- `include/note/arena.hpp`, `include/note/allocator.hpp`, `include/note/string_pool.hpp`, `include/note/transport_hal.hpp`, `include/note/streaming_transport.hpp`, `include/note/transport/cobs.hpp`
