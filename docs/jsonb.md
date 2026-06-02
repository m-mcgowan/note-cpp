# JSONB Wire Format

JSONB is Blues' binary TLV encoding for Notecard communication. It replaces JSON text with a compact binary format, reducing overhead and flash memory use on constrained devices.

## When to use JSONB

JSONB is useful when:
- You're building on a constrained device where JSON text parsing is expensive
- Your requests contain many numeric fields (no number formatting overhead)
- You want slightly smaller responses (no quote characters, no `:` separators)

JSON remains the default. JSONB is opt-in via a compile-time flag.

## Enabling JSONB

[`NOTE_MINIMAL`](feature-flags.md) enables JSONB automatically — no extra flags needed for constrained targets:

```ini
# platformio.ini — JSONB is on by default with NOTE_MINIMAL
build_flags = -DNOTE_MINIMAL
```

To enable JSONB explicitly on non-MINIMAL builds:

```cmake
target_compile_definitions(my_app PRIVATE NOTE_JSONB=1)
```

To opt out of JSONB on a MINIMAL build:

```ini
build_flags = -DNOTE_MINIMAL -DNOTE_JSONB=0
```

When `NOTE_JSONB=1`, the transport automatically encodes requests in JSONB and decodes JSONB responses. The typed API (`nc.card.version()`, `nc.hub.set()`, etc.) works identically -- the wire format is transparent to application code.

## Composes with both response presentations

JSONB is independent of the streaming-vs-tree response choice. The same `nc.card.status().execute()` call works in any of these four combinations:

```cpp
// 1. JSON × streaming (default ctor, no backend)
Notecard nc(transport);

// 2. JSON × tree
CjsonBackend backend;
Notecard nc(backend, transport);

// 3. JSONB × streaming — build with -DNOTE_JSONB=1
Notecard nc(transport);

// 4. JSONB × tree — build with -DNOTE_JSONB=1
CjsonBackend backend;
Notecard nc(backend, transport);
```

In tree mode under JSONB, the backend assembles the response tree directly from the SAX events the JSONB parser emits — there is no JSON-text round-trip in between. See [`composition.md`](composition.md) for the full matrix and [`examples/stdcpp/wire-format-and-presentation.cpp`](../examples/stdcpp/wire-format-and-presentation.cpp) for a working walk through all four cells.

## Wire framing

JSONB messages use a distinct framing to distinguish them from JSON:

```
{:<COBS-encoded JSONB opcodes>:}\n
```

- `{:` header (2 bytes) -- distinguishes JSONB from JSON (which starts with `{"`)
- COBS-encoded payload -- eliminates `\n` bytes so the newline terminator works
- `:}` trailer + `\n` terminator

The COBS variant uses XOR byte `0x0A` (newline), same as `note-cpp`'s existing binary transfer COBS implementation.

## Size comparison

For a `card.version` request:

| Format | Request | Response |
|--------|---------|----------|
| JSON   | 23 bytes | ~390 bytes |
| JSONB  | 27 bytes | ~376 bytes |

Requests are slightly larger (opcode overhead + COBS expansion). Responses are smaller (no quotes, colons, commas). The benefit compounds for payloads with many numeric fields.

## CRC handling

JSONB does not use CRC. The `"crc":"XXXX:YYYYYYYY"` field is a JSON-specific mechanism. When `NOTE_JSONB` is enabled, CRC is bypassed entirely. COBS framing provides its own integrity guarantees.

## Limitations

- **No raw JSON embedding:** `add_raw()` is a no-op in JSONB mode. Raw JSON fragments (e.g., body lambdas that emit pre-formatted JSON) cannot be embedded in JSONB without conversion.
- **Compile-time only:** The wire format is selected at compile time via `NOTE_JSONB`. Runtime switching is not supported.
- **Firmware requirement:** Requires Notecard firmware 11.x or later. Earlier firmware does not support JSONB.

## Internals

The JSONB implementation adds three components to `note-cpp`:

| Component | Header | Purpose |
|-----------|--------|---------|
| `StreamingJsonbBuilder` | `note/jsonb.hpp` | `JsonBuilder` that emits JSONB opcodes |
| `jsonb_parse_streaming()` | `note/jsonb.hpp` | SAX parser for JSONB opcode streams |
| `CobsStreamWriter` | `note/jsonb.hpp` | Streaming COBS encoder as a `JsonWriter` |

These slot into the existing transport pipeline without changing the notecard or generated API code. The `JsonBuilder` and `JsonSink` interfaces are the abstraction boundaries: the request builder writes opcodes through `JsonWriter` and the response parser fires the same `JsonSink` events the JSON lexer emits, so the rest of the library never sees the wire format.

Tree-mode response decode under JSONB goes through the backend's `start_response()` / `finish_response()` SAX-events-in interface; the same path JSON-text responses take. There is no separate "JSONB → text → tree" round-trip — the backend assembles the tree directly from the JSONB SAX events.
