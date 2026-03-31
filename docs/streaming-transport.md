# Streaming Transport

The transport layer provides three request execution paths with different
allocation trade-offs. All three share the same `ITransport` interface.

## Transport Interface

```
ITransport
  transact(request, timeout) → Result<string_view>   // JSON request/response
  send(request) → Result<void>                        // fire-and-forget
  write(data, len) → Result<void>                     // raw byte write
  read(buf, max_len, timeout) → Result<size_t>        // raw byte read
  reset()                                             // drain + resync
  abort()
```

`write()` and `read()` are byte-stream primitives — no framing, CRC, or
line terminators. Used for binary (COBS) streaming.

`transact()` and `send()` handle the full JSON protocol: wire buffer
preparation, CRC, retry, and response parsing.

## Execution Paths

### Path 1: Legacy (`transact()` with internal buffers)

```cpp
note::Notecard nc(backend, transport);
auto rsp = nc.card.version().execute();
```

- Uses `std::string wire_` and `response_buf_` inside `AbstractTransport`
- CRC check operates in-place on char buffers (no temporary `std::string`)
- Returns `string_view` into `response_buf_`
- Requires `<string>` — guarded by `#ifndef NOTE_NO_STD_STRING`

### Path 2: Caller-provided buffers (`set_receive_buffer` / `transact_into`)

```cpp
char buf[512];
transport.set_receive_buffer(buf, sizeof(buf));
auto rsp = nc.card.version().execute();  // reads into buf
```

Or one-shot:
```cpp
auto rsp = transport.transact_into(request, timeout, buf, sizeof(buf));
```

- Zero heap allocation for the receive path
- `response_buf_` bypassed entirely
- CRC check in-place on the caller buffer
- `wire_` still uses `std::string` for CRC append (see Remaining Work)

### Path 3: Streaming SAX (`transact_streaming` + `sax_parse_streaming`)

```cpp
VersionSink sink;
auto err = note::sax_parse_streaming(read_fn, timeout, sink);
```

- Zero-copy: reads 64-byte chunks, parses tokens as they arrive
- `JsonSink` callbacks populate response fields incrementally
- No intermediate buffer for the full response
- String values accumulated into a small scratch buffer (`SaxStreamBuf`)
- Verified on real hardware (ESP32-S3 + Notecard over serial)

### Binary path (`write` / `read` + COBS)

```cpp
api.card.binary.put().data(buf, len).execute();
```

- `do_binary_send`: COBS encode → stream via `write()`, MD5 verify
- `do_binary_receive`: stream via `read()` → `CobsDecoder.feed()`, MD5 verify
- 64-byte stack chunk — no intermediate buffer for the COBS stream

## CRC Handling

CRC operates on char buffers — no `std::string` involved:

```cpp
// Append CRC to outbound request (in-place)
wire_len_ = crc_add(wire_data(), wire_len_, wire_capacity(), crc_seq_);

// Verify and strip CRC from inbound response (in-place)
crc_check_and_strip(buf, len, expected_seq, crc_enabled);
```

Auto-detection: `crc_enabled` flips to `true` when the first valid CRC
field is found in a response. All subsequent responses must have CRC.

## `NOTE_NO_STD_STRING` Guard

Code that depends on `<string>` or `<functional>` is guarded:

```cpp
#ifndef NOTE_NO_STD_STRING
    std::string wire_;
    std::string response_buf_;
    virtual Result<void> do_receive(std::string& buf, uint32_t timeout_ms) = 0;
    // ... legacy transact, streaming transact, binary pipeline ...
#endif
```

When `NOTE_NO_STD_STRING` is defined, only Path 2 (`transact_into` /
`set_receive_buffer`) is available. This enables compilation on platforms
without `<string>` (e.g. AVR with polyfills from the compat project).

## Remaining Work

### Outbound path (not streamed)

Requests are always fully buffered before transmission:

1. `JsonBuilder::to_view()` builds the request into the builder's buffer
2. `prepare_wire()` copies it into `wire_` and appends CRC
3. `do_transmit()` sends the complete wire buffer

This is necessary because CRC is computed over the complete request body.
Streaming the outbound path would require incremental CRC (see below).

`wire_` is currently `std::string`. It could be replaced with:
- A caller-provided buffer via `set_wire_buffer()`
- A fixed-size member array (requests are typically <512 bytes)

### Backend dissolution

Wire `sax_parse_streaming` into `Notecard::execute()` as the default
parse path. The `JsonBackend` abstraction becomes optional — SAX replaces
tree parsing. The architecture simplifies to:

```
Developer code
  ↕  typed fields (C++ structs, ResponseField<T>)
SAX stream
  ↕  events (on_string, on_number, on_bool, ...)
Transport byte pipe
  ↕  raw bytes (read/write)
Notecard hardware
```

### Incremental CRC

Currently CRC requires the full response (computed over the body before
the CRC field). Incremental CRC (computed as bytes arrive) would eliminate
the last reason to buffer the full response.

## Resync After Errors

If a binary transfer fails mid-stream, bytes may remain in the transport.
`reset()` drains stale bytes — both I2C and serial implementations send
`\n` and wait for only control characters, matching note-c behavior.
`do_binary_receive()` calls `reset()` on failure before returning.

## Body Content Tiers

Response body content may be freeform JSON. Three tiers, allocation
always under developer control:

1. **SAX stream** — `JsonSink` callbacks, zero allocation
2. **`bodyAs<T>()`** — struct population via `NOTE_FIELDS`, zero intermediate allocation
3. **JSON tree** — explicit `bodyAsJson()`, developer pays for allocation
