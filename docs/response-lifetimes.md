# Response Lifetimes

Response fields (`string_view`, `bool`, `int32_t`, etc.) are populated
from the transport buffer when `execute()` returns. For non-string types,
the value is copied — it's yours forever. For `string_view` fields
(device IDs, version strings, error messages), the view points into the
transport's internal buffer.

## Default: valid until the next request

By default, `string_view` response fields are valid until the next
`execute()` or `request()` call on the same `Notecard`, because the
transport reuses its buffer:

```cpp
auto r = nc.card.version().execute();
auto ver = r.version;   // string_view — valid now

nc.hub.set().execute();  // transport buffer reused
// ver is now dangling — don't use it
```

For most code this is fine — you read the response, act on it, and
move on. The response goes out of scope naturally.

## Extending lifetime with StringPool

If you need response strings to survive beyond the next request, use
`set_allocator()` with a `MonotonicArena`. Response `string_view` fields
are copied into the arena and remain valid for the arena's lifetime:

```cpp
uint8_t buf[256];
note::MonotonicArena arena(buf, sizeof(buf));
nc.set_allocator(note::Allocator(arena));

auto r = nc.card.version().execute();
auto ver = r.version;   // interned into arena — null-terminated, survives transport reuse

nc.hub.set().execute();  // transport buffer reused
// ver is still valid — it lives in the arena
```

The arena is a simple bump allocator — no fragmentation, no free. Reset
it when you're done with all the interned strings:

```cpp
arena.reset();  // all interned strings invalidated
```

## When you need it

- **Storing multiple responses** — reading several endpoints and comparing
  their string fields afterward
- **Background processing** — passing a response to another task/thread
  that outlives the request scope
- **Caching** — keeping the last known device ID or firmware version

## When you don't

- **Immediate use** — read the field, print it, act on it, done
- **Non-string fields** — `bool`, `int32_t`, `double` are copied values,
  not views. They're always safe.
- **Typed structs** — `.into(T&)` copies field values into the struct.
  The struct owns its data.

## See also

- `examples/zero_alloc.cpp` — demonstrates arena-backed string interning
- `note::MonotonicArena` — the arena allocator
- `note::StringPool` — the interning helper used by `execute()`
- `note::Allocator` — the allocator interface passed to `set_allocator()`
