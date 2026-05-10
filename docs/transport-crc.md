# CRC

CRC protects JSON transactions against wire corruption. It is handled
transparently — no application code needed.

## Wire format

```
{"req":"card.version","crc":"001A:A1B2C3D4"}
                       └─── 22 bytes ─────┘
```

`SSSS` is a 4-hex-digit sequence number. `CCCCCCCC` is the CRC32 of the
JSON body (without the CRC field, without the trailing newline).

## Auto-detection

CRC is off by default. If the Notecard includes a `"crc"` field in any
response, CRC is enabled for all subsequent requests in that session. This
matches note-c's `notecardFirmwareSupportsCrc` behavior — the host
discovers CRC support from the Notecard, not from configuration.

Error responses (`"err"` field present) bypass CRC validation, matching
note-c behavior.

## Implementation

CRC is handled at two levels, depending on the transport path:

**Streaming path** (`Protocol`): CRC is accumulated
incrementally. On send, a `CrcWriter` wraps the `JsonWriter` and appends
the CRC suffix after the closing brace. On receive, a `CrcAccumulator`
feeds on bytes as they arrive, and a `CrcFieldSink` in the SAX sink chain
extracts the CRC field. `Protocol` compares accumulated vs
extracted values.

**Tree path** (`AbstractTransport`): CRC uses the in-place buffer
functions from `note/link/detail/crc32.hpp` — `crc_add()` appends
CRC to the wire buffer, `crc_check_and_strip()` validates and removes it
from the response.

Both paths share the same auto-detection and format. CRC sequence numbers
are fixed for all retries of a given request (matches note-c behavior).

## Disabling CRC

Define `NOTE_NO_CRC` to remove CRC support entirely (~200 B flash
savings). Set automatically by `NOTE_MINIMAL`.

See [feature flags](feature-flags.md) for all compile-time options.

## Test coverage

The CRC implementation and test suite are ported directly from note-c and
track upstream changes. See `tests/test_transport_crc32.cpp`.
