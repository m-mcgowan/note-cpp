# Memory Management

`note-cpp` can operate with **zero heap allocations** in steady state. This guide explains the memory model, when strings need interning, and how to choose the right configuration.

## Response String Lifetime

Response fields like `r.version` and `r.device` are `string_view`s — they point into memory owned by the JSON backend or transport layer, not into the `Response` struct itself.

**Without StringPool:** string_views point into the transport buffer and are valid until the next `execute()` call. This is fine when you consume response fields immediately:

```cpp
auto r = api.card.version().execute();
std::printf("version: %.*s\n", (int)r.version.size(), r.version.data());
// r.version is valid here — no second execute() has happened yet
```

**With StringPool:** string_views are copied into a `MonotonicArena` and survive transport buffer reuse. Use this when response data must outlive the next request:

```cpp
// Store the allocator once — all execute() calls use it automatically
char arena_buf[1024];
note::MonotonicArena arena(arena_buf, sizeof(arena_buf));
nc.set_allocator(note::arena_allocator(arena));

auto r1 = api.card.version().execute();
auto r2 = api.hub.status().execute();
// r1.version is still valid — it lives in the arena, not the transport buffer
```

### When Do You Need StringPool?

| Scenario | StringPool needed? |
|----------|-------------------|
| Read response, act on it, then make next request | No |
| Store response fields for later comparison | **Yes** |
| Collect data from multiple requests before processing | **Yes** |
| Fire-and-forget commands (no response) | No |
| Error messages from `r.error().message` with allocator set | Auto-interned |

**Rule of thumb:** if a response `string_view` must outlive the next `execute()` call, set an allocator.

## Backend Memory Profiles

| Backend | JSON building | JSON parsing | Heap allocs (steady state) |
|---------|--------------|-------------|---------------------------|
| `BufferJsonBackend<N,T>` | Fixed `char[N]` buffer | Fixed `jsmn_tok[T]` array | **0** |
| `CjsonArenaBackend` | cJSON (arena-backed) | cJSON (arena-backed) | **0** (arena) |
| `CjsonBackend` | cJSON (heap) | cJSON (heap) | ~5-10 per request |
| `NlohmannBackend` | nlohmann (heap) | nlohmann (heap) | Many per request |

For zero-allocation operation, use `BufferJsonBackend` or `CjsonArenaBackend`.

## Configuration

### Minimal (no StringPool)

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport_fn);
note::Api api(nc);

auto r = api.card.version().execute();  // 0 heap allocs
// Use r.version before next execute()
```

### With StringPool (strings survive reuse)

```cpp
note::backends::BufferJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport_fn);

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

## See Also

- [examples/zero_alloc.cpp](../examples/zero_alloc.cpp) — working example of all three patterns
- [docs/json-backend.md](json-backend.md) — backend selection and customization
- `include/note/arena.hpp` — `MonotonicArena` implementation
- `include/note/allocator.hpp` — `Allocator` type and adapters
- `include/note/string_pool.hpp` — `StringPool` implementation
