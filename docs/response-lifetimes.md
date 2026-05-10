# Response Lifetimes

`execute()` returns a response with typed fields. Some of those fields are **values** — numbers, booleans — and some are **views** — `string_view` fields like `r.version` or `r.device` that point at memory the library owns.

**The rule:**

- **Value fields** (`bool`, `int32_t`, `double`, etc.): copied into the response struct. Yours forever. Safe.
- **`string_view` fields**: valid **until the next `execute()` call on the same `Notecard`** — unless you set an arena allocator, in which case they're valid until the arena is reset.

This doc explains when that matters and how to extend the lifetime if you need to.

## The default case

```cpp
auto r = nc.card.version().execute();
auto ver = r.version;        // string_view — points into the transport buffer
std::printf("%s\n", ver.c_str());

nc.hub.set().execute();      // transport buffer gets reused
// ver is now dangling — don't use it
```

For most code this is fine: read the response, act on it, move on. If a response field only lives inside one logical step, you don't need an allocator.

## Extending lifetime with an arena

If you need response strings to survive the next request, set an **arena allocator** on the `Notecard`. The library will copy each response `string_view` into the arena, and the views you see will point into that arena instead of the transport buffer.

```cpp
uint8_t buf[256];
note::MonotonicArena arena(buf, sizeof(buf));
nc.set_allocator(note::arena_allocator(arena));

auto r = nc.card.version().execute();
auto ver = r.version;       // a copy was made into the arena

nc.hub.set().execute();     // transport buffer reused, but ver is unaffected
std::printf("%s\n", ver.c_str());    // still valid
```

An arena is a simple bump allocator: it hands out memory linearly, and the only way to free is to drop everything at once with `reset()`.

```cpp
arena.reset();               // every arena-backed string_view is now invalid
nc.set_allocator(note::arena_allocator(arena));  // re-bind after reset
```

After `reset()`, don't touch any `string_view` you read earlier — they point into memory that's about to be overwritten.

The same mechanism is called **interning** in the source code: the library "interns" each response string by copying it into the arena and handing you a view into the copy. The interned copy is null-terminated, which is why `c_str()` works on arena-backed response fields.

## When you need an arena

- **Comparing multiple responses** — read several endpoints, then do something that depends on more than one of their string fields.
- **Background processing** — pass a response to another task or thread that outlives the function that made the request.
- **Caching** — keep the last known device ID or firmware version across requests.

## When you don't

- **Immediate use** — read the field, print it, act on it, done.
- **Non-string fields** — `bool`, `int32_t`, `double`, enums. These are always safe.
- **Typed-body parsing with `.into(T&)`** — copies field values directly into your struct. The struct owns its data.

## `.into(T&)` and string field types

When streaming a response body into a user struct via `.into(cfg)`, the
field type decides ownership:

| Field type            | Behaviour                                                          | Lifetime                               |
|-----------------------|--------------------------------------------------------------------|----------------------------------------|
| `note::string_view`   | View over the transport / arena buffer (same as response fields).  | Until next `execute()` or arena reset. |
| `std::string`         | Copied into a self-owned heap buffer.                              | As long as the struct is alive.        |
| `char[N]`             | `memcpy` with explicit null terminator; truncates to `N-1` bytes.  | As long as the struct is alive.        |
| Arduino `String`      | Copied if the core exposes `String(const char*, size_t)`. Stock AVR Arduino does; others vary. | As long as the struct is alive. |
| `const char*`         | **Not supported** — silently skipped.                              | N/A                                    |

The `const char*` case can't work safely because there's no buffer the
library owns to point at — the input string_view is transient. Use one
of the owning types above instead.

Unsupported field types are silently skipped today; a `static_assert`
gate behind a macro is tracked as a follow-up design question.

## See also

- [memory.md](memory.md) — full memory model, backend profiles, streaming vs. tree
- [arena-sizing.md](arena-sizing.md) — computing the right arena size at compile time
- [examples/stdcpp/zero-alloc.cpp](../examples/stdcpp/zero-alloc.cpp) — working example
- `include/note/arena.hpp` — `MonotonicArena` implementation
- `include/note/string_pool.hpp` — the interning helper used by `execute()`
- `include/note/allocator.hpp` — `Allocator` type and `arena_allocator()` adapter
