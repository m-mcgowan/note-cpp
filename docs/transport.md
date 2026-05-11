# Transport

`note-cpp` ships a complete, header-only implementation of both Notecard Serial and I2C wire protocols, with the timing, CRC and framing that Notecard expects — equivalent to note-c, with no global state and no cJSON dependency.

This page covers the choices you make when constructing a `Notecard`: which JSON layer mode (sink vs tree), which constructor, the per-protocol guides, the Arduino shorthand, and how retry is wired in.

> For the layered architecture (8 layers, OSI mapping, per-layer headers), see [`docs/internal/streaming-transport.md`](internal/streaming-transport.md). That's contributor-grade material; you don't need it to use the library.

## Transport-agnostic API

Both layers above `Notecard` are transport-agnostic. The same call
yields equivalent results regardless of which transport the Notecard
was constructed against:

- **Typed API** (`api.note.read().into(struct).execute()`,
  `api.note.update(file, id).body(struct).execute()`).
- **Raw JSON API** (`nc.transact(json, buf)`, `nc.send(json)`).

> Contributor Note: This is pinned in CI by `tests/test_transport_agnostic_api.cpp`, which pairs four call-site categories against both Notecard ctors:

| § | Surface | Streaming | Tree |
|---|---|:---:|:---:|
| 1 | `api.note.read().into(struct).execute()` | ✓ | ✓ |
| 2 | `api.note.update(file, id).body(struct).execute()` | ✓ | ✓ |
| 3 | `nc.transact(json, buf)` | ✓ | ✓ |
| 4 | `nc.send(json)` | ✓ | ✓ |

If the high-level surface ever drifts apart between transports, one of
those four pairs goes red.

## JSON layer — streaming or tree

The streaming-vs-tree choice is a JSON-layer concern, not a transport one: it controls how `Notecard` turns response bytes into typed values.

| Mode | How it parses | Enables | Memory profile |
|------|---|---|---|
| **Tree mode** | `JsonReader` walks a parsed tree | `response.body()` returns `JsonReader*` for ad-hoc walking | Builds a tree (jsmn tokens or cJSON nodes) sized to the response |
| **Streaming mode** | SAX events fire into `Rsp::Sink` | `.into(T&)` populates user struct directly | Zero intermediate tree |

Both modes populate the typed `Response` struct identically. The
mode is selected by *which `Notecard` ctor* you use:

```cpp
// Tree mode — JsonBackend supplied → response.body() works.
note::backends::StaticJsonBackend<512, 64> backend;
note::Notecard nc(backend, transport);

// Streaming mode — no JsonBackend → smaller flash, .into(struct) for body.
note::Notecard nc(transport, note::Allocator{});
```

`.into(T&)` works in both modes (transport-agnostic, see § 1 above).
`response.body()` is tree-mode only — streaming-mode has no tree to walk.

### Mode selection guide

Pick **tree mode** when:
- You're migrating from note-c — the `request()` lambda builder matches
  the familiar "build JSON, send, parse" pattern.
- You need ad-hoc JSON walking via `JsonReader`.
- You're debugging wire traffic and want a tree to inspect.

Pick **streaming mode** when:
- You're on a memory-constrained target — no tree, no JsonBackend
  pulled in, smaller flash.
- All your body shapes are known statically (use `.into(T&)`).
- You don't need `response.body()` for any endpoint.

### Comparison

| Feature | Tree mode | Streaming mode |
|---------|:---------:|:---------:|
| Typed `execute()` on requests | yes | yes |
| Typed response fields | yes | yes |
| `.into(T&)` body parse into struct | yes | yes |
| `.body(T&)` send struct as body | yes | yes |
| `nc.transact(json, buf)` raw JSON | yes | yes |
| Binary transfers (COBS) | yes | yes |
| Error handling (`ApiResult`) | yes | yes |
| `request()` lambda builder | yes | — |
| `response.body() -> JsonReader*` | yes | — |
| Requires `JsonBackend` | yes | no |
| Zero-heap capable | depends on backend | yes |

Define `NOTE_NO_BUFFERED` to remove tree mode entirely (~2-4 KB flash savings). Set automatically by `NOTE_MINIMAL`.

For the tree-mode backend matrix (`CjsonBackend`, `StaticJsonBackend`, `CjsonArenaBackend`, `NlohmannBackend`) — when each fits and how to wire it up — see [json-backend.md](json-backend.md).

## Transport guides

| Guide | Covers |
|-------|--------|
| [Serial transport](transport-serial.md) | `SerialHal`, Arduino setup, protocol constants, binary streaming |
| [I2C transport](transport-i2c.md) | `I2cHal`, MTU negotiation, priming query, Arduino setup |
| [CRC](transport-crc.md) | Auto-detection, wire format, streaming vs tree implementation |
| [Binary transfer](binary-transfer.md) | `card.binary` put/get, COBS, MD5 verification |
| [JSONB wire format](jsonb.md) | Compact binary encoding (alternative to JSON text) |

## Arduino shorthand

On Arduino, `note::arduino::Notecard` wraps the full stack behind
`begin()`. Streaming mode by default; pass a `JsonBackend&` to opt into
tree mode:

```cpp
#include <note.hpp>

note::arduino::Notecard nc;

void setup() {
    nc.begin(Serial1, 9600);                // streaming mode, serial
    // or: nc.begin(Wire);                   // streaming mode, I2C
    // or: nc.begin(Wire, 0x17);             // streaming mode, I2C with custom address
    // or: nc.begin(Serial1, 9600, backend); // tree mode (response.body() works)
    // or: nc.begin(Wire, backend);          // tree mode, I2C
}
```

The tree-mode begin overloads no longer take a separate `rsp_buf`
argument — the Notecard owns a default response staging buffer
(`NOTE_RSP_BUF_SIZE`, default 1024 bytes). Call
`nc.set_response_buffer(span)` after `begin()` if you need a
non-default size.

See the [Arduino guide](platforms/arduino/guide.md) for full setup
details.

## Retry

Retry is handled by the `Notecard` layer, not the transport. Each
request carries a `Safety` level (`ReadOnly`, `Idempotent`,
`NonIdempotent`, `Destructive`) that gates retry behaviour:

- `SendFailed` — always retried (request never reached the Notecard).
- `ResponseLost` — retried only for `ReadOnly` and `Idempotent`
  requests.
- Notecard errors — never retried.

See [retry design](internal/retry-design.md) for the full safety
matrix and importance levels.
