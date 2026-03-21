# Binary Transfer

The Notecard supports transferring raw binary data between the host MCU and the
Notecard's binary store via a COBS-encoded channel. note-cpp handles one segment
of this transfer at a time — COBS encoding/decoding, MD5 compute/verify, and the
JSON handshake are all handled invisibly inside `execute()`.

Multi-segment orchestration (reset → append loop → finalize) is the concern of
[note-cpp-app](https://github.com/blues/note-cpp-app).

---

## API

### PUT: host → Notecard binary store

```cpp
// Layer 1 — explicit offset:
auto rsp = api.card.binaryPut()
    .offset(int32_t(0))    // must match card.binary.length on the Notecard (append-only)
    .buffer(data, len)     // synthetic field: triggers binary pipeline
    .execute();

// Layer 2 — offset optional (defaults to 0):
auto rsp = api.binary.put(data, len).execute();
auto rsp = api.binary.put(data, len, /*offset=*/N).execute();
```

`buffer` is a synthetic field — it is **not** serialized to JSON. Its presence
tells `execute()` to run the binary pipeline instead of normal JSON dispatch.

`offset` must match the Notecard's current `card.binary.length`. Data can only
be appended to the binary store, never overwritten. The caller is responsible
for tracking the offset across multiple calls.

### GET: Notecard binary store → host

```cpp
// Layer 1 — explicit offset and length:
note::byte_span rx_buf{buf, sizeof(buf)};
auto rsp = api.card.binaryGet()
    .offset(int32_t(0))
    .length(int32_t(len))
    .buffer(rx_buf)        // synthetic: destination for decoded bytes
    .execute();
// rsp.buffer → span<const uint8_t> into rx_buf, sized to bytes received

// Layer 2 — offset optional (defaults to 0):
auto rsp = api.binary.get(buf, len).execute();
auto rsp = api.binary.get(buf, len, /*offset=*/N).execute();
```

### DFU

`dfu.get` uses the same COBS pipeline as `card.binary.get` and the same buffer
field mechanism.

### note.add with binary: true

`note.add { binary: true, live: true }` tells the Notecard to forward the
contents of its binary store to Notehub. It is **not** a host→Notecard upload —
no synthetic buffer field is needed. The data must already be in the binary store
via `binaryPut()` before calling this.

Routing between `payload` (base64, ≤256 bytes) and the binary store (larger
payloads) is a policy decision that belongs in note-cpp-app, not here.

---

## What execute() does (one segment)

MD5 is computed on **raw (pre-COBS) bytes**, consistent with note-c. COBS
encoding happens afterwards.

### PUT pipeline

```
1. card.binary.get          JSON: verify max >= data.size()
2. compute_md5(data)        raw bytes → md5_provider_.compute()
3. COBS encode data         streaming encoder
4. card.binary.put          JSON: cobs=encoded_len, offset=N, status=md5_hex
5. raw COBS bytes + '\n'    transport chunked transmit
6. card.binary.get          JSON: verify response.status matches md5_hex
```

### GET pipeline

```
1. card.binary.get          JSON request: offset=N, length=L
                             JSON response: status=md5_of_decoded  (no length in response)
2. raw COBS bytes           transport chunked receive
3. COBS decode into dst     streaming decoder
4. verify decoded_len == L  mismatch → error
5. compute_md5(dst[0..L])   raw decoded bytes → verify vs response.status
```

`rsp.buffer` is a span into the caller's destination buffer, sized to the bytes
actually decoded (normally equal to the requested `length`; may be smaller on
partial receive).

To know the data size before calling `binaryGet`, issue a `card.binary` status
call first — its response has `length` (unencoded bytes in store), `cobs`
(COBS-encoded size), and `status` (MD5 of the full buffer). The `card.binary.get`
response itself only carries `status` (MD5) and `err`.

A failed MD5 or length verification returns an error `ApiResult`. No retries at
this level — retry policy is a broader deferred discussion.

---

## MD5 provider

MD5 is not a transport concern. It is injected into `Notecard` as a separate
`Md5Provider` interface, independent of the transport HAL:

```cpp
class Md5Provider {
public:
    /// Compute MD5 of raw bytes; return as lowercase hex string.
    virtual std::string compute(const uint8_t* data, size_t len) = 0;
    virtual ~Md5Provider() = default;
};
```

The library ships two implementations selected at compile time:

```cpp
// Always available — pure C++ software implementation:
class SoftwareMd5 : public Md5Provider { ... };

// When mbedtls is present (detected via __has_include):
#if __has_include(<mbedtls/md5.h>)
class MbedTlsMd5 : public Md5Provider { ... };
using PlatformMd5 = MbedTlsMd5;
#else
using PlatformMd5 = SoftwareMd5;
#endif
```

`Notecard` defaults to `PlatformMd5`. Platforms can inject a custom
implementation:

```cpp
note::Notecard nc(hal, transport_fn, command_fn);           // PlatformMd5
note::Notecard nc(hal, transport_fn, command_fn, &my_md5);  // custom override
```

---

## Layer division

| Layer | Responsibility |
|-------|----------------|
| **note-cpp** | One segment: COBS encode/decode, MD5 compute+verify, chunked streaming, JSON handshake |
| **note-cpp-app** | Multi-segment: reset (offset=0), append loop, offset tracking, DFU orchestration |

---

## Memory management

note-cpp avoids heap allocations throughout. Binary transfer follows the same
principle — no hidden allocations, caller controls buffer placement.

### COBS codec buffer

The encoder and decoder each use a ~256-byte working buffer. By default this is
stack-allocated. For stack-constrained targets, pass a single `span<uint8_t>`
to `execute()` — it is shared between encoder and decoder (they never run at
the same time) and no stack buffer is used:

```cpp
// Default — stack buffer, no extra code:
api.binary.put(data, len).execute();
api.binary.get(buf, len).execute();

// Stack-constrained — one static buffer shared by both directions:
static uint8_t cobs_buf[NOTE_COBS_BLOCK_SIZE];
api.binary.put(data, len).execute({cobs_buf, sizeof(cobs_buf)});
api.binary.get(buf, len).execute({cobs_buf, sizeof(cobs_buf)});
```

`NOTE_COBS_BLOCK_SIZE` is 255 by default and can be overridden at build time
before including any note headers.

### Sizing helpers

```cpp
// Maximum COBS-encoded size for a given raw payload length:
size_t scratch = note::cobs_encoded_size(raw_len);  // raw_len + raw_len/254 + 1
```

Use `note::cobs_encoded_size()` when you need to know upfront how much space the
Notecard will need to store a payload (e.g. to compare against `card.binary`
response `max` field).
