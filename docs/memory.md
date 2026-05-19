# Memory management

`note-cpp` can run with **zero heap allocations** in steady state — important on embedded targets without a heap, or where you just don't want to pay for `malloc` on every request.

This guide covers two questions that come up as soon as you go past "hello world":

1. **"How long are response strings valid?"** — fields like `r.version` are views, not copies; they become invalid at some point.
2. **"What memory knobs do I have if I'm flash/RAM-constrained?"** — backends, arenas, streaming vs. tree.

The short version for both: **by default things work**. You only need this guide when the default isn't enough.

## Do I need to do anything special?

| Scenario | What to do |
|----------|------------|
| Read a response, use it immediately, drop it | **Nothing.** The default allocator (heap-backed) frees the response's interned strings when the `Response` goes out of scope. |
| Fire-and-forget commands (no response) | Nothing. |
| Collect data from several requests before processing them as a batch | Arena allocator with one `reset()` at the end of the batch. Avoids per-Response malloc/free churn on the hot path. |
| Running on a target with no `malloc` available | Arena allocator. The library never calls `malloc` directly; the `Allocator` you supply is the only source of dynamic memory. |
| Running on a target with no heap / strict flash budget | Use the streaming path with an arena, or `StaticJsonBackend`. See [Backend profiles](#backend-memory-profiles). |
| Sharing response strings with other components after the `Response` is destroyed | Copy each field into `std::string` before the Response goes out of scope, or use an arena and keep the arena alive. |

If you're in the first row, stop reading. If you're anywhere else, continue.

## Picking an allocator

Every `Notecard` carries one `Allocator` (set at construction or via `set_allocator()`). The allocator is where response string interning happens — the SAX parser copies wire strings into allocator-backed storage so the views in your typed `Response` outlive the transport buffer.

Each `Response` captures its allocator (by value) and releases its interned strings in its destructor — that's the **Phase 1 RAII contract** the library guarantees regardless of which allocator you picked. The difference between allocators is where the bytes come from, what `deallocate` does when the Response dtor runs, and what other lifecycle hooks (`reset()`) you have available.

| Allocator | When to use | Lifetime of response strings | What `~Response()` does |
|---|---|---|---|
| **Default (heap-backed)** — `Notecard nc(transport);` or `Notecard(backend, transport)` with no `set_allocator` | Desktop, prototyping, short-running scripts. Anywhere `malloc`/`free` is available and you don't need bounded RAM. | Valid until the `Response` goes out of scope; the destructor calls `free` on every interned string. No accumulation across `execute()` calls. | One `free` per interned string field. |
| **`MonotonicArena`** — `note::arena_allocator(arena)` | Embedded targets, long-running services, anywhere you want bounded and predictable memory use. The arena can live on the stack, in `.bss`, or in a member buffer. | Valid until `arena.reset()` (you call it). After reset, every view that pointed into the arena is invalid even if the Response is still in scope. | No-op — the arena's `deallocate` does nothing; `arena.reset()` is what reclaims memory. |
| **`HeapResetPool`** — `note::heap_reset_allocator(pool)` | Desktop / Linux hosts that want arena-style "drain on reset" semantics without sizing a buffer up front. Storage comes from `malloc`; `pool.reset()` (or destruction) frees everything in one pass. | Valid until `pool.reset()` or the pool's destructor runs. | No-op — same shape as MonotonicArena; cleanup batched at `reset()`. |
| **`std::pmr`** — `note::pmr_allocator(&resource)` (C++17+) | Mixed projects already using `std::pmr::memory_resource`. Lets you reuse a `monotonic_buffer_resource`, a `synchronized_pool_resource`, or your own. | Determined by the resource's lifetime. | Whatever the resource's `deallocate` does — per-block free for pool resources, no-op for monotonic resources. |
| **Custom function-pointer** — fill in `note::Allocator{ alloc, free, realloc, ctx }` | RTOS pool, locked region, instrumented allocator for tests. | You decide. | Whatever your `free` function does. |

The library never calls `malloc`/`new` of its own accord — `Allocator` is the only entry point. That makes "where does response memory come from" a one-line decision you can audit at the construction site.

The reason all five options share the same surface is that the Response captures the allocator value at parse time and uses it during destruction. Moving a `Response` (which `ApiResult<T>` does on the return path) transfers ownership; copying is deleted, so you can't accidentally end up with two Responses sharing the same string allocations. See `tests/test_allocator_lifetime.cpp` for the executable form of this contract — the alloc and free counters move in lockstep on every allocator backend.

## Response string lifetimes

Response fields like `r.version`, `r.device`, and most string fields are `std::string_view` — pointers into memory the library owns, not copies. They're cheap, but they have an expiry date that depends on which allocator the Notecard is using and how the `Response` is held.

The high-level rule is **the `Response` owns its strings**: when the `Response` (or the `ApiResult<T>` wrapping it) goes out of scope, the destructor releases every interned string back to the allocator that minted them.

Mode-by-mode:

- **Tree mode, no `set_allocator`:** strings live inside the JSON reader, which is replaced on the next `execute()`. Views from the previous call become invalid as soon as the next request runs.
- **Default heap allocator (any mode):** strings live until the `Response` goes out of scope. The dtor calls `free` on each one; nothing accumulates across calls.
- **Arena allocator (any mode):** strings live in the arena buffer. The Response dtor calls `deallocate` (a no-op for arenas), so views stay valid as long as the arena hasn't been `reset()` — even after the Response has been moved or destroyed.

The arena is the "keep this string around past the Response" mechanism. With the default heap allocator the `Response` *is* the keep-alive scope; with an arena the *arena* is.

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

- **Streaming path** — builds the request directly into the transport and parses the response with a SAX parser as bytes arrive. No request/response buffers needed. Heap usage is whatever the configured `Allocator` does (zero with `MonotonicArena`, `malloc` per interned string with the default heap allocator). Pair with an arena for bounded RAM.
- **Tree path** — builds the full request in a `JsonBackend`, sends it, reads the full response back, then parses into a walkable `JsonReader`. Simpler mental model. Heap allocation depends on the backend.

See [streaming-and-tree.md](streaming-and-tree.md) for a side-by-side comparison and when to choose each.

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
- [streaming-and-tree.md](streaming-and-tree.md) — which path to pick
- [json-backend.md](json-backend.md) — backend selection and customization
- [binary-transfer.md](binary-transfer.md) — binary transfer memory model
