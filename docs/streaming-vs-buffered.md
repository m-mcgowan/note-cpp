# Streaming vs Buffered

`note-cpp` has two internal paths for building requests and parsing responses:

- **Streaming** (default) — requests are serialized directly to the wire, responses are SAX-parsed as bytes arrive. No intermediate buffer, no heap allocation.
- **Buffered** — requests are built into a string buffer, responses are parsed into a JSON tree via a [JSON backend](json-backend.md) (cJSON, nlohmann, or `BufferJsonBackend`). A raw string interface (`transact()`) is also available for passing pre-built JSON.

The typed API (`execute()`, response fields, body structs) works identically on both paths — you don't need to change application code when switching between them.

## When to use buffered

You might prefer the buffered path when:

- **Migrating from note-c** — existing code builds JSON with cJSON (`J*`
  functions). The buffered path lets you keep that pattern while gradually
  adopting typed requests:

  ```cpp
  auto reader = nc.request("hub.set", [](JsonBuilder& b) {
      b.add("product", "com.example.app");
      b.add("mode", "periodic");
  });
  ```

- **Manual JSON tree inspection** — if you need to walk an unknown or
  dynamic response structure (not a typed endpoint), `JsonReader` from
  the buffered path gives tree-style access.

- **Debugging** — the intermediate JSON string is visible in debuggers.

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

## What's different

| Feature | Streaming | Buffered |
|---------|:---------:|:--------:|
| `request()` with lambda builder | — | yes |
| `JsonReader` tree access on responses | — | yes |
| Requires a `JsonBackend` | no | yes |
| Intermediate request/response buffer | none | full string |
| Zero-heap capable | yes | no (cJSON uses malloc) |

## Wire format

By default, `note-cpp` uses JSON text on the wire — the same format as
note-c. On constrained platforms, enable [JSONB](jsonb.md) (`NOTE_JSONB`
or `NOTE_MINIMAL`) to use a compact binary encoding instead. JSONB is
the same binary format used by Blues'
[note-c-zero](https://github.com/blues/note-c-zero) library. Your
application code doesn't change — only the bytes on the wire are different.

## Disabling the buffered path

Define `NOTE_NO_BUFFERED` to remove the buffered path entirely, saving
~2-4 KB flash. This is set automatically by `NOTE_MINIMAL`.

See [feature-flags.md](feature-flags.md) for all compile-time options.

## Choosing a JSON backend (buffered path only)

| Backend | Heap | Best for |
|---------|:----:|----------|
| `CjsonBackend` | yes | Migration from note-c, matches cJSON allocation model |
| `NlohmannBackend` | yes | Projects already using nlohmann/json |
| `BufferJsonBackend<N, M>` | no | Fixed-size buffer, no heap, but uses static RAM |

See [json-backend.md](json-backend.md) for backend configuration details.
