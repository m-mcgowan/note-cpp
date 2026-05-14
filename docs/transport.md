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

The transport stack delivers response bytes to the JSON layer, which turns those bytes into typed values. There are two strategies the JSON layer can use, and the choice is decided once at `Notecard` construction time. In **streaming mode**, a SAX parser fires events directly into your typed response struct; nothing is held in memory after the call. In **tree mode**, a `JsonBackend` builds a walkable `JsonReader` on the response that you can query by key after the call returns.

For most of the typed API the choice is invisible: the request builders, the response field accessors, `.into(struct)` body parsing, raw `nc.transact()`, and binary transfers all behave identically in either mode. The modes diverge on post-call body inspection (`response.body()`), the lambda request builder (`nc.request(...)`), and on whether a `JsonBackend` needs to be linked at all.

For the full comparison, selection guide, and backend matrix, see [streaming-and-tree.md](streaming-and-tree.md).

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
