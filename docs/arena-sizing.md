# Memory Management with Arenas

`note-cpp` uses a `MonotonicArena` for dynamic memory during response parsing — string fields are interned into the arena rather than heap-allocated. This gives you full control over memory usage with zero heap allocations.

## Arena Sizing

Each generated `Response` struct carries a `static constexpr max_arena_size` — the minimum arena buffer needed to hold all string data from that response type. Use this to statically size your arena:

### What goes in the arena

**String fields** — `ResponseField<string_view>` values are *interned* into the
arena by `StringPool::intern()` during SAX parsing. Each interned string costs
its byte length, rounded up to alignment.

**Error string** — the error message (if any) is interned into the arena.
Budget: 64 bytes (matches `ErrorCaptureSinkT::kMaxErrLen`).

### What does NOT go in the arena

**Primitives** — `bool`, `int32_t`, and `double` response fields are stored
directly in the `Response` struct on the stack. The streaming SAX parser delivers
these as native values (`on_bool`, `on_int`, `on_float`) with no intermediate
buffer or conversion — they are written straight into `ResponseField<T>`. Zero
arena cost, zero conversion overhead.

**Body primitives** — when a response includes a `body` object, the SAX parser
delivers body fields directly to the user-provided struct via `StructSink<T>`.
Primitive body fields (`bool`, `int32_t`, `double`) have zero arena cost.
Body string fields are interned into the user's own `StringPool`, not this arena.
The `into(T&)` call on the request builder wires up the struct sink before
execution — no arena budget is needed for body parsing here.

### Alignment overhead

Each arena allocation is aligned to `alignof(std::max_align_t)` = **16 bytes**
on all modern platforms (ARM Cortex-M, ESP32, AVR, x86-64). The `arena_cost(n)`
function rounds `n` up to the next multiple of 16:

```
arena_cost(n) = (n + 15) & ~15    // 0 → 0, 1..16 → 16, 17..32 → 32, ...
```

## String Length Assumptions

Default maximum lengths by wire name, used when `x-max-length` is not set in the
spec. Fields not listed use the default of **48 bytes**.

| Wire name | Max length | Aligned cost | Notes |
|-----------|-----------|-------------|-------|
| `payload` | 256 | 256 | |
| `movements` | 128 | 128 | |
| `text` | 128 | 128 | |
| `status` | 80 | 80 | |
| `why` | 80 | 80 | |
| `email` | 64 | 64 | |
| `host` | 64 | 64 | |
| `product` | 64 | 64 | |
| `file` | 48 | 48 | |
| `name` | 48 | 48 | |
| `note` | 48 | 48 | |
| `org` | 48 | 48 | |
| `ssid` | 48 | 48 | |
| `zone` | 48 | 48 | |
| `version` | 40 | 48 (rounds up from 40) | |
| `board` | 32 | 32 | |
| `device` | 32 | 32 | |
| `format` | 32 | 32 | |
| `mode` | 32 | 32 | |
| `role` | 32 | 32 | |
| `sn` | 32 | 32 | |
| `security` | 24 | 32 (rounds up from 24) | |
| `sku` | 24 | 32 (rounds up from 24) | |
| `area` | 16 | 16 | |
| `method` | 16 | 16 | |
| `country` | 8 | 16 (rounds up from 8) | |
| *(default)* | 48 | 48 | any unlisted wire name |

## Per-Endpoint Arena Sizes

Sorted by `max_arena_size` descending. Error reserve (64 bytes) is always included.

| Request | String fields | Error | **Total** |
|----------|-------------|-------|-----------|
| `CardAttn::Request` | `files[8]`=48×8, `payload`=256 | 64 | **704** |
| `CardAttn::Query` | `files[8]`=48×8 | 64 | **448** |
| `DfuGet` | `payload`=256, `status`=80 | 64 | **400** |
| `WebDelete` | `payload`=256, `status`=80 | 64 | **400** |
| `WebPost` | `payload`=256, `status`=80 | 64 | **400** |
| `WebPut` | `payload`=256, `status`=80 | 64 | **400** |
| `HubGet` | `device`=32, `host`=64, `mode`=32, `product`=64, `sn`=32, `vinbound`=48, `voutbound`=48 | 64 | **384** |
| `CardAttn::Retrieve` | `payload`=256 | 64 | **320** |
| `CardRandom` | `payload`=256 | 64 | **320** |
| `NoteGet::Pop` | `payload`=256 | 64 | **320** |
| `NoteGet::Read` | `payload`=256 | 64 | **320** |
| `Web` | `payload`=256 | 64 | **320** |. why is Web smaller than WebDelete?
| `WebGet` | `payload`=256 | 64 | **320** |
| `CardMotion` | `mode`=32, `movements`=128, `status`=80 | 64 | **304** |
| `CardContact::Get` | `email`=64, `name`=48, `org`=48, `role`=32 | 64 | **256** |
| `CardContact::Set` | `email`=64, `name`=48, `org`=48, `role`=32 | 64 | **256** |
| `CardVersion` | `board`=32, `device`=32, `name`=48, `sku`=24, `version`=40 | 64 | **256** |
| `CardBinary::Clear` | `err`=48, `status`=80 | 64 | **192** |
| `CardBinary::Status` | `err`=48, `status`=80 | 64 | **192** |
| `CardBinaryGet` | `err`=48, `status`=80 | 64 | **192** |
| `CardWifi` | `security`=24, `ssid`=48, `version`=40 | 64 | **192** |
| `EnvGet` | `text`=128 | 64 | **192** |
| `NtnStatus` | `err`=48, `status`=80 | 64 | **192** |
| `VarGet` | `text`=128 | 64 | **192** |
| `CardLocation` | `mode`=32, `status`=80 | 64 | **176** |
| `DfuStatus` | `mode`=32, `status`=80 | 64 | **176** |
| `HubSyncStatus` | `mode`=32, `status`=80 | 64 | **176** |
| `CardLocationMode::Continuous` | `mode`=32, `vseconds`=48 | 64 | **144** |
| `CardLocationMode::Get` | `mode`=32, `vseconds`=48 | 64 | **144** |
| `CardLocationMode::Periodic` | `mode`=32, `vseconds`=48 | 64 | **144** |
| `CardLocationMode::Remove` | `mode`=32, `vseconds`=48 | 64 | **144** |
| `CardLocationMode::Set` | `mode`=32, `vseconds`=48 | 64 | **144** |
| `CardStatus` | `status`=80 | 64 | **144** |
| `CardTime` | `area`=16, `country`=8, `zone`=48 | 64 | **144** |
| `CardWireless` | `status`=80 | 64 | **144** |
| `CardWirelessPenalty::Check` | `status`=80 | 64 | **144** |
| `CardWirelessPenalty::Clear` | `status`=80 | 64 | **144** |
| `CardWirelessPenalty::Set` | `status`=80 | 64 | **144** |
| `HubStatus` | `status`=80 | 64 | **144** |
| `CardBinaryPut` | `err`=48 | 64 | **112** |
| `CardDfu` | `name`=48 | 64 | **112** |
| `CardLocationTrack` | `file`=48 | 64 | **112** |
| `NoteAdd` | `note`=48 | 64 | **112** |
| `CardAux` | `mode`=32 | 64 | **96** |
| `CardAuxSerial::Request` | `mode`=32 | 64 | **96** |
| `CardCarrier` | `mode`=32 | 64 | **96** |
| `CardLocationMode::Fixed` | `mode`=32 | 64 | **96** |
| `CardSleep` | `mode`=32 | 64 | **96** |
| `CardTriangulate` | `mode`=32 | 64 | **96** |
| `CardVoltage::Configure` | `mode`=32 | 64 | **96** |
| `CardVoltage::Read` | `mode`=32 | 64 | **96** |
| `NoteTemplate::Define` | `format`=32 | 64 | **96** |
| `NoteTemplate::Remove` | `format`=32 | 64 | **96** |
| `CardTransport` | `method`=16 | 64 | **80** |
| `CardAttn::Arm` | *(none)* | 64 | **64** |
| `CardAttn::Rearm` | *(none)* | 64 | **64** |
| `CardIllumination` | *(none)* | 64 | **64** |
| `CardPower::Configure` | *(none)* | 64 | **64** |
| `CardPower::Read` | *(none)* | 64 | **64** |
| `CardPower::Reset` | *(none)* | 64 | **64** |
| `CardTemp::Configure` | *(none)* | 64 | **64** |
| `CardTemp::Read` | *(none)* | 64 | **64** |
| `CardTemp::Stop` | *(none)* | 64 | **64** |
| `CardUsageGet` | *(none)* | 64 | **64** |
| `CardUsageTest` | *(none)* | 64 | **64** |
| `EnvModified` | *(none)* | 64 | **64** |
| `EnvSet` | *(none)* | 64 | **64** |
| `EnvTemplate` | *(none)* | 64 | **64** |
| `FileChanges` | *(none)* | 64 | **64** |
| `FileChangesPending` | *(none)* | 64 | **64** |
| `FileStats` | *(none)* | 64 | **64** |
| `HubSignal` | *(none)* | 64 | **64** |
| `NoteChanges::Peek` | *(none)* | 64 | **64** |
| `NoteChanges::Pop` | *(none)* | 64 | **64** |
| `NtnGps` | *(none)* | 64 | **64** |

### Void-response endpoints (no arena cost)

`CardAttn::Disarm`, `CardAttn::Off`, `CardAttn::On`, `CardAttn::Sleep`, `CardAttn::Watchdog`, `CardAuxSerial::Configure`, `CardAuxSerial::Gps`, `CardAuxSerial::Notify`, `CardAuxSerial::Off`, `CardIo`, `CardLed`, `CardMonitor`, `CardMotionMode`, `CardMotionSync`, `CardMotionTrack`, `CardRestart`, `CardRestore`, `CardTrace`, `EnvDefault::Remove`, `EnvDefault::Set`, `FileClear`, `FileDelete`, `HubLog`, `HubSet`, `HubSync`, `NoteDelete`, `NoteUpdate`, `NtnReset`, `VarDelete`, `VarSet`

## Summary

| Metric | Value |
|--------|-------|
| Endpoints with `max_arena_size` | 75 |
| Void-response endpoints | 30 |
| Largest response | `CardAttn::Request` — 704 bytes |
| Smallest (with strings) | `CardTransport` — 80 bytes |
| Error-reserve only (no strings) | 21 endpoints — 64 bytes each |
| Default string max length | 48 bytes |
| Alignment | 16 bytes (`alignof(std::max_align_t)`) |

## RequestSet Examples

`RequestSet::max_arena_size` is the **maximum** of its members — allocate once, reuse for any request.

### Minimal sensor loop

```cpp
using Requests = note::RequestSet<note::api::CardTemp::Read, note::api::CardVoltage::Read, note::api::CardStatus, note::api::HubStatus>;
// max_arena_size = 144
```

- `CardTemp::Read`: 64 bytes
- `CardVoltage::Read`: 96 bytes
- `CardStatus`: 144 bytes
- `HubStatus`: 144 bytes

### Note I/O with body parsing

```cpp
using Requests = note::RequestSet<note::api::NoteAdd, note::api::NoteGet::Read, note::api::NoteGet::Pop>;
// max_arena_size = 320
```

- `NoteAdd`: 112 bytes
- `NoteGet::Read`: 320 bytes
- `NoteGet::Pop`: 320 bytes

### ATTN with scoped operations (vs full Request)

```cpp
using Requests = note::RequestSet<note::api::CardAttn::Arm, note::api::CardAttn::Query, note::api::CardStatus>;
// max_arena_size = 448
```

- `CardAttn::Arm`: 64 bytes
- `CardAttn::Query`: 448 bytes
- `CardStatus`: 144 bytes

