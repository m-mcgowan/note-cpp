# Glossary

This page bridges two vocabularies: the **Notecard wire vocabulary** that readers see in the [Blues docs](https://dev.blues.io/) and the **`note-cpp` library vocabulary** they need to map onto when reading the rest of these docs. One-line definitions throughout; each entry links into the deeper-coverage doc when there is one.

## Notecard wire vocabulary

These are the terms a reader meets first on dev.blues.io and in the Notecard's own JSON request/response shape.

- **Notecard** — the cellular/Wi-Fi/LoRa device that an embedded host talks to over serial or I2C.
- **Notehub** — the Blues-hosted cloud service that Notecards sync with. Routes data to your application via webhooks, MQTT, or other integrations.
- **Note** — a single JSON document — the unit of data exchanged between host, Notecard, and Notehub. A Note's payload lives under the `body` key.
- **Notefile** — a named queue of Notes on the Notecard (e.g. `sensors.qo`). The suffix encodes direction and durability: `.qo` outbound queue, `.qi` inbound queue, `.db` two-way database.
- **Inbound / outbound** — direction relative to the host. Outbound Notes flow host → Notecard → Notehub; inbound Notes flow Notehub → Notecard → host.
- **`req` vs `cmd`** — wire-level request shapes. `{"req":...}` expects a response; `{"cmd":...}` is fire-and-forget — the Notecard executes it without replying.
- **Body** — the user-defined JSON payload nested under the `"body"` key in a Note. Application data lives here; everything else is metadata.
- **Template** — a typed schema you register against a Notefile (`note.template`) so the Notecard stores Notes in a compact binary form instead of full JSON. See [body-values.md](body-values.md).
- **SKU** — the hardware part number (e.g. `NOTE-WBNA`). Selects radio, region, and capabilities. Used by `note-cpp`'s targeting filter to gate API surface — see [target-filtering example](../examples/stdcpp/target-filtering.cpp).

## `note-cpp` library vocabulary

These are the terms used throughout this codebase's docs and headers — the layer above the wire vocab.

- **Typed API** — methods and fields generated from the Notecard schema, e.g. `nc.card.version().execute()`. The default surface; see [using-the-api.md § The three layers](using-the-api.md#the-three-layers).
- **Lambda request builder** — `nc.request("endpoint", [](auto& b) { ... })`. Hand-built JSON with the same retry/transport plumbing as the typed API; see [using-the-api.md § Lambda request builder](using-the-api.md#lambda-request-builder).
- **Raw JSON** — `nc.transact(json_string, buf)`. Strings in, strings out — the thinnest wrapper. See [using-the-api.md § Raw JSON](using-the-api.md#raw-json).
- **Focused operation** — one method per intent on a multi-purpose Notecard endpoint, e.g. `nc.note.read()` vs `nc.note.pop()` for `note.get`. Each operation exposes only the fields that apply, plus a retry-safety classification. See [using-the-api.md § Focused operations](using-the-api.md#focused-operations-on-multi-purpose-endpoints).
- **Tree mode** — JSON layer strategy that parses each response into a `JsonReader` tree (`response.body()` returns a walkable pointer). Requires a `JsonBackend`. See [transport.md § JSON layer](transport.md#json-layer-streaming-or-tree).
- **Streaming mode** — JSON layer strategy that fires SAX events directly into the response sink, no intermediate tree. Smaller flash, no `JsonBackend` linked. See [transport.md § JSON layer](transport.md#json-layer-streaming-or-tree).
- **Arena** — a bump allocator (`MonotonicArena`) you hand a fixed buffer; it serves linear allocations and frees them all at once on `reset()`. The "keep response strings alive past the next call" mechanism. See [memory.md](memory.md).
- **`Hal`** — platform byte conduit (`transmit`, `read`, `reset`, `millis`, `delay`). You implement this for your platform; the library does the rest. See [internal/streaming-transport.md § Architecture](internal/streaming-transport.md#architecture).
- **`Protocol`** — the concrete Notecard wire protocol driver: CRC, init handshake, line termination, sequence numbers, framing over a `Hal`. See [internal/streaming-transport.md § Architecture](internal/streaming-transport.md#architecture).
- **`ITransact`** — unified Notecard transaction interface: `transact(req, span)`, `transact(req, sink)`, `send(req)`. The contract a session class holds; `Protocol` implements it natively. See [internal/streaming-transport.md § Transport Interfaces](internal/streaming-transport.md#transport-interfaces).
- **Session** — runtime object owning the transport, optional `JsonBackend`, retry policy, and inter-transaction timing. Where retry happens. The session classes (`Notecard`, `BareNotecard`, `StaticNotecard`) are peers, not stacked.
- **Backend** (`JsonBackend`) — JSON tree-mode strategy (cJSON, nlohmann, fixed buffer + jsmn, cJSON-on-arena). See [json-backend.md](json-backend.md).
- **Sink** (`JsonSink`) — SAX-event receiver from the streaming parser; the streaming-mode counterpart to `JsonBackend`. Each typed `Response` defines one (`Rsp::Sink`); user code rarely writes one directly.
- **`StaticNotecard`** — peer session class that wires the transport and (optional) backend at compile time, no virtual dispatch. Smallest flash; no runtime swap.
- **`BareNotecard`** — peer session class that strips retry and inter-transaction timing. Use when you handle those concerns yourself.
- **`Api<>`** — the generated typed surface, templated on a session class. `nc.card.version()` etc. live here.
- **`NotecardApi`** — convenience bundle: a default `Notecard` plus an `Api<>` in one object so callers don't construct both. The 99% case on stdcpp.
- **`StringPool`** — interns response strings into the arena and guarantees null-termination. Every typed-response `string_view` field is backed by it. See [memory.md](memory.md).
- **`ApiResult<Response>`** — what a typed `execute()` call returns. `if (r)` checks success; `r.error()` returns an `ErrorInfo` on failure. See [error-handling.md](error-handling.md).
- **`Safety`** — per-operation retry classification: `ReadOnly`, `Idempotent`, `NonIdempotent`, `Destructive`. The library uses it automatically; you can also static-assert it. See [error-handling.md](error-handling.md).
- **`BodyValue`** — typed body field on responses. Carries the parsed `body` JSON; in tree mode walks via `JsonReader`, in streaming mode populated via `.into(T&)`. See [body-values.md](body-values.md).
- **`JsonView`** — substring scanner over a raw JSON string buffer. Skips ~8 KB of flash on AVR vs the SAX sink path; trades robust parsing for size. See [using-the-api.md § Raw JSON](using-the-api.md#raw-json).
- **JSONB** — compact binary encoding the Notecard accepts in place of JSON text. Auto-enabled by `NOTE_MINIMAL`. See [jsonb.md](jsonb.md).

## See also

- [Blues Notecard documentation](https://dev.blues.io/) — canonical reference for wire vocabulary
- [`docs/using-the-api.md`](using-the-api.md) — full discussion of the typed API, its layers, and escape hatches
- [`docs/memory.md`](memory.md) — full discussion of arenas, string pools, and lifetimes
- [`docs/internal/streaming-transport.md`](internal/streaming-transport.md) — `Hal`, `Protocol`, `ITransact`, and the layered architecture
