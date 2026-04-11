# JSONB Wire Format

JSONB is Blues' binary TLV encoding for Notecard communication. It replaces JSON text with compact binary opcodes, reducing overhead for numeric-heavy payloads and eliminating number-to-string conversion at both ends.

## When to use JSONB

JSONB is useful when:
- Your requests contain many numeric fields (no number formatting overhead)
- You want slightly smaller responses (no quote characters, no `:` separators)
- You're building on a constrained device where JSON text parsing is expensive

JSON remains the default. JSONB is opt-in via a compile-time flag.

## Enabling JSONB

`NOTE_MINIMAL` enables JSONB automatically -- no extra flags needed for constrained targets:

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

When `NOTE_JSONB=1`, `StreamingTransport` automatically encodes requests in JSONB and decodes JSONB responses. The typed API (`nc.card.version()`, `nc.hub.set()`, etc.) works identically -- the wire format is transparent to application code.

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

These slot into the existing `StreamingTransport` pipeline without changing the transport, notecard, or generated API code. The `JsonBuilder` and `JsonSink` interfaces are the abstraction boundaries.
