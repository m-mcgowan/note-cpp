# Memory management

`note-cpp` can run with **zero heap allocations** in steady state — important on embedded targets without a heap, or where you just don't want to pay for `malloc` on every request.

This guide covers two questions that come up as soon as you go past "hello world":

1. **"How long are response strings valid?"** — fields like `r.version` are views, not copies; they become invalid at some point.
2. **"What memory knobs do I have if I'm flash/RAM-constrained?"** — backends, arenas, streaming vs. tree.

The short version for both: **by default things work**. You only need this guide when the default isn't enough.

## Do I need to do anything special?

| Scenario | What to do |
|----------|------------|
| Read a response, use it immediately, call the next request | **Nothing.** Default works. |
| Keep response fields around past the next `execute()` | Set an **arena allocator** (below). |
| Collect data from several requests before processing | Arena allocator. |
| Fire-and-forget commands (no response) | Nothing. |
| Using the streaming path | Arena is **required** by the constructor — the library won't compile without one. |
| Running on a target with no heap / strict flash budget | Use the streaming path, or `StaticJsonBackend`. See [Backend profiles](#backend-memory-profiles). |

If you're in the first row, stop reading. If you're anywhere else, continue.

## Response string lifetimes

Response fields like `r.version`, `r.device`, and most string fields are `std::string_view` — pointers into memory the library owns, not copies. They're cheap, but they have an expiry date.

Three rules:

1. **Default (tree mode, no arena):** views are valid **until the next `execute()` call**. This is the common case — read, use, move on.
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

## Choosing a path: streaming vs. tree

`note-cpp` has two execution paths. Pick one based on your transport and memory constraints:

- **Streaming path** — builds the request directly into the transport and parses the response with a SAX parser as bytes arrive. No request/response buffers needed. **Always zero heap.** Requires an arena (passed to the constructor).
- **Tree path** — builds the full request in a `JsonBackend`, sends it, reads the full response back, then parses into a walkable `JsonReader`. Simpler mental model. Heap allocation depends on the backend.

See [transport.md](transport.md#streaming-vs-tree) for a side-by-side comparison and when to choose each.

## Backend memory profiles

On the tree path, your backend choice decides whether you allocate heap memory per request:

| Backend | JSON build | JSON parse | Heap allocs per request |
|---------|------------|------------|-------------------------|
| *Streaming (no backend)* | direct-to-transport | SAX | **0** |
| `StaticJsonBackend<N,T>` | fixed `char[N]` | fixed token array | **0** |
| `CjsonArenaBackend` | cJSON + arena | cJSON + arena | **0** (all from arena) |
| `CjsonBackend` | cJSON (heap) | cJSON (heap) | ~5–10 |
| `NlohmannBackend` | nlohmann (heap) | nlohmann (heap) | many |

For zero-alloc: streaming path, or `StaticJsonBackend`, or `CjsonArenaBackend`.

## Configuration recipes

### Streaming mode — zero heap (recommended for embedded)

No `JsonBackend`: requests build directly to the wire, responses
SAX-parse into `Rsp::Sink` and (when set) `.into(struct)`.

```cpp
note::link::SerialFramer serial_hal(hal);    // Hal
note::Protocol transport(serial_hal);               // ITransact (protocol)

char arena_buf[256];
note::MonotonicArena arena(arena_buf);
note::Notecard nc(transport, note::arena_allocator(arena));
note::Api api(nc);

auto r = api.card.version().execute();   // 0 heap allocs, strings in arena
// r.version valid until arena.reset()
```

No backend needed. No `std::string` linked. No `operator new`.

### Tree mode — with `StaticJsonBackend` (zero heap)

A `JsonBackend` enables `response.body()` to return a walkable
`JsonReader` tree.

```cpp
note::backends::StaticJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);   // ITransact (same Protocol)
note::Api api(nc);

auto r = api.card.version().execute();   // 0 heap allocs
// Consume r before the next execute()
```

The two template parameters are: request/response buffer size, and jsmn token count. Pick values that fit your largest request and largest response.

### Tree path — with arena

```cpp
note::backends::StaticJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);

char arena_buf[1024];
note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
nc.set_allocator(note::arena_allocator(arena));

note::Api api(nc);
auto r = api.card.version().execute();   // 0 heap allocs, strings in arena
// r.version valid until arena.reset()
```

## Beyond this page

Binary transfer follows the same caller-owns-buffer model — `.data()` / `.into()` on `card.binary.{put,get}` take your buffer, and COBS encoding/decoding happens in place with no scratch allocation. Full treatment in [binary-transfer.md](binary-transfer.md), including the `set_cobs_buffer()` hook for stack-constrained targets.

The streaming SAX parser's internals — `sax_parse_streaming()`, the 384-byte default `SaxStreamBuf`, the read/key/value scratch partitioning — are documented in [internal/streaming-transport.md](internal/streaming-transport.md).

## Allocation proof

The integration tests override global `operator new`/`operator delete` to count every C++ heap allocation:

- `tests/integration/buffer/test_alloc_profile` — proves `StaticJsonBackend` + tree parse = 0 heap allocs.
- `tests/integration/buffer/test_sax_alloc_profile` — proves streaming + arena = 0 heap allocs and strings survive transport reuse.

## See also

- [examples/zero-alloc.cpp](../examples/stdcpp/zero-alloc.cpp) — working example of the patterns above
- [response-lifetimes.md](response-lifetimes.md) — in-depth guide to `string_view` validity
- [arena-sizing.md](arena-sizing.md) — computing arena size at compile time
- [transport.md](transport.md#streaming-vs-tree) — which path to pick
- [json-backend.md](json-backend.md) — backend selection and customization
- [binary-transfer.md](binary-transfer.md) — binary transfer memory model
