# Binary Transfer

The Notecard supports transferring binary data between the host MCU and the
Notecard's binary store via a COBS-encoded channel. `note-cpp` handles one segment
of this transfer at a time and handles the protocol details - COBS encoding/decoding, MD5 compute/verify, and the
JSON handshake are all handled.

---

## API

### PUT: host → Notecard binary store

```cpp
// Attach data and execute — binary pipeline triggered automatically:
auto rsp = api.card.binary.put()
    .data(buf, len)             // attach source data
    .offset(int32_t(0))        // append position in Notecard's store
    .execute();                 // COBS encode + stream + verify

// Skip post-transmit verification for performance:
auto rsp = api.card.binary.put()
    .data(buf, len)
    .verify(false)
    .execute();

// Top-level alias:
api.binary.put().data(buf, len).execute();
```

The `.data()` method attaches a source buffer. Its presence triggers the binary
pipeline in `execute()` — COBS encoding, MD5 computation, streaming, and
post-transmit verification all happen automatically. Without `.data()`,
`execute()` sends a normal JSON request (useful for manual control).

### GET: Notecard binary store → host

```cpp
auto rsp = api.card.binary.get()
    .into(dst, sizeof(dst))     // attach destination buffer
    .length(int32_t(N))         // bytes to retrieve
    .execute();                 // stream + COBS decode + MD5 verify

// Top-level alias:
api.binary.get().into(dst, sizeof(dst)).length(N).execute();
```

The `.into()` method attaches a destination buffer. `execute()` streams COBS
data from the transport, decodes it into the destination buffer, and verifies
the MD5 against the Notecard's response.

### Status and clear

```cpp
auto status = api.binary.status().execute();
// status.max — maximum bytes the Notecard can store
// status.length — bytes currently stored (unencoded)
// status.cobs — COBS-encoded size of stored data
// status.status — MD5 of stored data

api.binary.clear().execute();
```

### Post-transmit verification

PUT requests include post-transmit verification by default. After streaming the
COBS data, `execute()` queries `card.binary` status and confirms the Notecard's
stored MD5 matches what was sent. If the Notecard discarded the data (corruption
on the wire), the MD5 won't match and `execute()` returns an error.

This costs one extra JSON round-trip. Disable it for latency-sensitive paths:

```cpp
api.binary.put().data(buf, len).verify(false).execute();
```

GET requests verify MD5 locally (no extra round-trip) — the MD5 from the JSON
response is compared against the decoded data. This is always on.

### DFU

`dfu.get { binary: true }` tells the Notecard to stage a firmware chunk into
the `card.binary` buffer. The host then retrieves it via a normal
`card.binary.get` call. There is no direct COBS streaming from `dfu.get`.

### note.add with binary

`note.add { binary: true }` tells the Notecard to forward the contents of its
binary store to Notehub. It is **not** a host→Notecard upload — no buffer
attachment is needed. The data must already be in the binary store via
`card.binary.put`.

---

## What execute() does (one segment)

MD5 is computed on **raw (pre-COBS) bytes**, consistent with note-c.

### PUT pipeline

```
1. cobs_encoded_length(data)   exact encoded size (O(n) scan)
2. md5_provider.compute(data)  MD5 of raw bytes
3. card.binary.put             JSON handshake: cobs=N, status=md5_hex, offset=M
4. COBS encode → write()       stream encoded blocks to transport
5. write('\n')                  EOP byte
6. card.binary                  verify: Notecard's stored MD5 matches (if verify=true)
```

### GET pipeline

```
1. card.binary.get             JSON request: length=L, offset=M
                                JSON response: status=md5_of_data
2. read() → CobsDecoder        stream + decode into destination buffer
3. md5_provider.compute(dst)   verify decoded data matches response status
```

---

## MD5 provider

MD5 is injected into `Notecard` as a separate `Md5Provider` interface:

```cpp
class Md5Provider {
public:
    virtual std::string compute(const uint8_t* data, size_t len) = 0;
    virtual ~Md5Provider() = default;
};
```

Two implementations ship with the library:

- `SoftwareMd5` — pure C++ (always available)
- `MbedTlsMd5` — hardware-accelerated (ESP32, detected via `__has_include`)

`PlatformMd5` aliases the best available. Custom implementations can be
injected via `nc.set_md5_provider(my_md5)`.

---

## Sizing helpers

```cpp
// Worst-case COBS encoded size (O(1), for buffer sizing):
size_t max = note::cobs_encoded_size(raw_len);

// Exact COBS encoded size for specific data (O(n), for JSON handshake):
size_t exact = note::cobs_encoded_length(data, raw_len);
```

`cobs_encoded_size()` is the worst case — use it for buffer allocation.
`cobs_encoded_length()` scans the data for zeros and returns the exact size —
use it for the `cobs` field in the JSON handshake (as `do_binary_send` does).
