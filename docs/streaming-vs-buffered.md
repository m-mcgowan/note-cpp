# Streaming vs Buffered API

`note-cpp` has two internal paths for communicating with the Notecard:
**streaming** (default) and **buffered**. From the user's perspective, the
typed API (`execute()`, response fields, body structs) works identically
on both — the choice only matters in specific situations.

## When to use streaming (default)

Streaming is the default and recommended path. Requests are serialized
directly to the wire, and responses are parsed with a SAX (event-driven)
parser that populates struct fields as bytes arrive. No intermediate
buffer is needed.

```cpp
// Arduino — streaming by default
#include <note.hpp>
Notecard nc;
nc.begin(Serial1, 9600);

auto rsp = nc.card.version().execute();  // streaming under the hood
```

**Use streaming when:**
- Starting a new project
- Targeting constrained devices (AVR, Cortex-M0)
- You want zero-heap operation
- You're using the typed API exclusively

## When to use buffered

The buffered path requires a JSON backend (cJSON, nlohmann, or
`BufferJsonBackend`). Requests are built into a string buffer, sent as a
whole, and responses are parsed into a JSON tree that you can walk manually.

```cpp
// Buffered — explicit backend + transport
#include <note/backends/cjson.hpp>
#include <note/notecard.hpp>

note::backends::CjsonBackend backend;
note::Notecard nc(backend, transport);
```

**Use buffered when:**

- **Migrating from note-c** — existing code builds JSON with cJSON (`J*`
  functions). The buffered path lets you keep that pattern while gradually
  adopting typed requests. The `request()` lambda method mirrors the
  note-c workflow:

  ```cpp
  auto reader = nc.request("hub.set", [](JsonBuilder& b) {
      b.add("product", "com.example.app");
      b.add("mode", "periodic");
  });
  ```

- **Manual JSON tree inspection** — if you need to walk an unknown or
  dynamic response structure (not a typed endpoint), `JsonReader` from
  the buffered path gives tree-style access. Streaming only provides
  typed fields or raw string passthrough.

- **Debugging** — the intermediate JSON string is visible in debuggers,
  which can help diagnose wire format issues.

## What's the same on both paths

| Feature | Streaming | Buffered |
|---------|:---------:|:--------:|
| Typed `execute()` on requests | yes | yes |
| Typed response fields | yes | yes |
| Body structs (`.body()`, `.into()`, `.bodyAs<T>()`) | yes | yes |
| Binary transfers (COBS) | yes | yes |
| Error handling (`ApiResult`, `ErrorInfo`) | yes | yes |
| `operator[]` for ad-hoc fields | yes | yes |
| Fire-and-forget `.command()` | yes | yes |
| Wire format identical | yes | yes |

## What's different

| Feature | Streaming | Buffered |
|---------|:---------:|:--------:|
| `request()` with lambda builder | — | yes |
| `JsonReader` tree access on responses | — | yes |
| Requires a `JsonBackend` | no | yes |
| Intermediate request/response buffer | none | full string |
| Allocator required | yes | optional |
| Zero-heap capable | yes | no (cJSON uses malloc) |

## Disabling the buffered path

Define `NOTE_NO_BUFFERED` to remove the buffered path entirely, saving
~2-4 KB flash. This is set automatically by `NOTE_MINIMAL`.

See [feature-flags.md](feature-flags.md) for all compile-time options.

## Choosing a JSON backend (buffered path only)

If you use the buffered path, you need a backend:

| Backend | Heap | Best for |
|---------|:----:|----------|
| `CjsonBackend` | yes | Migration from note-c, matches cJSON allocation model |
| `NlohmannBackend` | yes | Projects already using nlohmann/json |
| `BufferJsonBackend<N, M>` | no | Fixed-size buffer, no heap, but uses static RAM |

See [json-backend.md](json-backend.md) for backend configuration details.
