# Protocol Timing — note-c vs note-cpp

## Summary

Comparison of timing constraints in the Notecard wire protocol as
implemented by note-c and note-cpp. Covers intra-transaction pacing,
per-request timeouts, retry behaviour, readiness gating, and the
implicit overhead that affects inter-transaction cadence.

## Serial Transport

| Parameter | note-c | note-cpp | Match |
|-----------|--------|----------|-------|
| Segment max length | 250 bytes (`CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN`) | 250 bytes (`segment_max_len`) | Yes |
| Segment delay | 250ms (`CARD_REQUEST_SERIAL_SEGMENT_DELAY_MS`) | 250ms (`segment_delay_ms`) | Yes |
| Chunked transmit | Yes — `_serialChunkedTransmit` | Yes — `NotecardSerial::transmit()` chunks at segment_max_len | Yes |
| RX poll delay (idle) | 10ms (`_DelayMs(10)` when `!cardTurboIO`) | 1ms (`hal_.delay(1)`) | Faster |
| RX poll delay (after first byte) | 0ms — spins (`delay && received == 0`) | 1ms (same path) | Faster |
| Intra-transaction timeout | 1s (`CARD_INTRA_TRANSACTION_TIMEOUT_SEC`) | 1s (`intra_timeout_ms`) | Yes |
| Reset drain window | 500ms (`CARD_RESET_DRAIN_MS`) | 500ms (`reset_drain_ms`) | Yes |
| Reset sync retries | 10 | 10 (`reset_sync_retries`) | Yes |
| Max transaction retries | 5 (`CARD_REQUEST_RETRIES_ALLOWED`) | 5 (`max_retries`) | Yes |
| Retry delay | 500ms (`RETRY_DELAY_MS`) | 500ms (`retry_delay_ms`) | Yes |
| Line terminator | `\r\n` | `\r\n` | Yes |
| `cardTurboIO` (skip IO delays) | Yes — global flag | No equivalent | Missing |

## I2C Transport

| Parameter | note-c | note-cpp | Match |
|-----------|--------|----------|-------|
| Segment max length | 250 bytes (`CARD_REQUEST_I2C_SEGMENT_MAX_LEN`) | 250 bytes (`segment_max_len`) | Yes |
| Segment delay | 250ms (`CARD_REQUEST_I2C_SEGMENT_DELAY_MS`) | 250ms (`segment_delay_ms`) | Yes |
| IO delay (pre-operation) | 6ms (`_delayIO`, `!cardTurboIO`) | 6ms (`io_delay_ms`) | Yes |
| Chunk delay (inter-chunk) | 20ms (`CARD_REQUEST_I2C_CHUNK_DELAY_MS`) | 20ms (`chunk_delay_ms`) | Yes |
| NACK wait | 1000ms (`CARD_REQUEST_I2C_NACK_WAIT_MS`) | 1000ms (`nack_wait_ms`) | Yes |
| Response poll interval | 50ms | 50ms (`response_poll_ms`) | Yes |
| Intra-transaction timeout | 1s | 1s (`intra_timeout_ms`) | Yes |

## Per-Request Timeout Calculation

note-c computes the transaction timeout dynamically based on the
request type (`_noteTransaction_calculateTimeoutMs` in `n_request.c`):

| Request type | Timeout |
|-------------|---------|
| Default | `(CARD_INTER_TRANSACTION_TIMEOUT_SEC - 1) * 1000 + 1000` = 30s |
| `note.add` with `milliseconds` | that value + 1s |
| `note.add` with `seconds` | that value * 1000 + 1s |
| `web.*` with `milliseconds` | that value + 1s |
| `web.*` with `seconds` | that value * 1000 + 1s |

The +1s headroom lets the Notecard time out internally first and report
the timeout to the host, rather than the host aborting prematurely.

`NoteSetRequestTimeout(secs)` overrides the global default (the
`CARD_INTER_TRANSACTION_TIMEOUT_SEC` macro reads from
`cardTransactionTimeoutOverrideSecs`).

**note-cpp**: uses a single `default_timeout_ms_` (default 30s) for all
requests. No per-request override, no request-type inspection.

## Transaction Hooks (`_TransactionStart` / `_TransactionStop`)

note-c brackets every transaction with hook calls:

```
_TransactionStart(timeoutMs)   // before
  _LockNote()
  ... transaction ...
  _UnlockNote()
_TransactionStop()             // after
```

### What the hooks do

`_TransactionStart(timeoutMs)` calls `hookTransactionStart(timeoutMs)`.
Without hooks installed, it returns `true` immediately — **no delay, no
readiness check**. With hooks, the platform gets up to `timeoutMs` to
confirm that the Notecard is ready. The return value is boolean: `true`
= ready, `false` = timeout (request aborted with "Notecard not ready
(CTX/RTX)").

`_TransactionStop()` calls `hookTransactionStop()`. Without hooks, it's
a no-op.

Hooks are registered via `NoteSetFnTransaction(startFn, stopFn)`.

### CTX/RTX: deep sleep wake handshake

The only known implementation of these hooks is `NoteTxn_Arduino` in
note-arduino, which implements a **hardware GPIO handshake** for waking
the Notecard from deep sleep:

- **RTX** (Request To Transact) — host MCU output pin, driven HIGH to
  request communication
- **CTX** (Clear To Transact) — host MCU input pin, Notecard drives
  HIGH when awake and ready

Protocol: host asserts RTX → polls CTX with 1ms interval up to
`timeoutMs` → if CTX goes HIGH, proceed → on completion, float RTX
so the Notecard can sleep again.

From `note-arduino/src/Notecard.h`:

> Transaction pins are not necessary on any legacy Notecards, and are
> only necessary for certain Notecard SKUs.

This is a **niche feature for specific SKUs** that use aggressive deep
sleep. Most deployments never call `NoteSetFnTransaction` — meaning
`_TransactionStart` returns `true` immediately and note-c has **no
inter-transaction readiness check** in typical use.

### CTX/RTX hardware details

- **M.2 pins**: CTX = pin 49, RTX = pin 47
- **Voltage**: VIO_P (3.3V logic). Can be left disconnected if unused.
- **Used with both I2C and serial**: ESP32 deep sleep powers down all
  peripherals — both bus controllers are offline until wake.
- **SKU support**: primarily NOTE-WIFI v2 (ESP32-based) and NOTE-LORA.
  `card.sleep` (deep sleep mode) is WiFi-SKU only. Legacy cellular
  Notecards (STM32-based) use STOP mode where I2C/UART peripherals
  stay partially alive — bus activity itself can wake them, so CTX/RTX
  is unnecessary.
- **Notecarriers**: X v1.2, XS v1.1, XP v3.1 expose CTX/RTX pins.
  Older Notecarrier F does not.
- **Schema**: `card.version` response does not include a capability
  flag for CTX/RTX support. No schema-level indication of which SKUs
  require it.

### Two levels of readiness

| Level | Where | Purpose |
|-------|-------|---------|
| **Hardware wake** (CTX/RTX) | Transaction hooks | Is the Notecard powered up and listening? (deep sleep recovery) |
| **Transport sync** (reset) | Inside transport | Is the wire protocol in a clean state? (serial: `\n` → `\r\n`; I2C: zero-length read → ACK) |

Neither addresses the third case:

| Level | Where | Purpose |
|-------|-------|---------|
| **Application readiness** | Not implemented | Has the Notecard finished internal processing? (e.g. environment variable population after `card.attn` fires) |

In the application readiness case, the Notecard is awake (CTX/RTX
would pass), the transport is synced (reset would succeed), but the
requested data isn't available yet. The Notecard accepts the request
and responds — but with stale or empty data.

### What note-cpp doesn't have

- No transaction hooks (CTX/RTX or otherwise)
- No per-request timeout calculation
- No `_LockNote`/`_UnlockNote` mutex around transactions
- No inter-transaction gap to allow Notecard processing time

**Impact**: note-cpp's streaming path sends requests back-to-back with
sub-millisecond gaps. note-c's implicit overhead (malloc, J* parsing,
CRC) adds 1-5ms between transactions — enough for most cases. But
neither library has an explicit mechanism for application-level
readiness (e.g. the 2-second gap needed after `card.attn` fires before
`env.get` returns populated data).

## Retry Behaviour

note-c retries all requests blindly up to 5 times on any I/O error.
This is unsafe for non-idempotent requests like `note.add` — if the
request was processed but the response was lost, the retry creates a
duplicate note.

note-cpp's transport currently uses the same blind retry, but the
design (see `docs/retry-design.md`) uses phase-based error categories
and per-request `Safety` classification to gate retries:

| Error phase | ReadOnly | Idempotent | NonIdempotent | Destructive |
|-------------|----------|------------|---------------|-------------|
| `SendFailed` | Retry | Retry | Retry | Retry |
| `ResponseLost` | Retry | Retry | **No retry** | **No retry** |
| `Notecard` | No | No | No | No |

Only `SendFailed` (request never reached the Notecard) is universally
safe. `ResponseLost` (request sent, response not received) is only
retried for `ReadOnly`/`Idempotent` requests. See `safety.hpp` for
the classification and `error.hpp` for the error phases.

| Parameter | note-c | note-cpp |
|-----------|--------|----------|
| Max retries | 5 | 5 (transport-level; to be gated by Safety) |
| Retry delay | 500ms | 500ms |
| Reset on retry | Yes (`_Reset()`) | Yes (`hal_.reset()`) |
| CRC retry | Yes (500ms delay) | Not implemented |
| Safety-gated retry | No (blind) | Yes (design in `retry-design.md`) |

note-c additionally retries on:
- CRC mismatch (500ms delay)
- Null response when one was expected (500ms delay)
- Heartbeat responses (don't count against retry limit)

## Reset Sequence

### Serial

1. 250ms delay
2. `_SerialReset()` (platform hook)
3. Send `\n`
4. Drain for 500ms, reading all data, 1ms poll interval
5. If only `\r`/`\n` received: ready
6. If non-control chars or nothing: retry (up to 10 times)
7. Between retries: 500ms delay + `_SerialReset()`

### I2C

1. 250ms delay
2. `_I2CReset(address)` (platform hook)
3. 6ms IO delay
4. Send `\n` via `_I2CTransmit`
5. On transmit NACK: 1000ms wait, retry
6. 250ms delay for response
7. Drain for 500ms, reading chunks with 20ms delays
8. If only `\r`/`\n` received: ready
9. If non-control chars: retry (up to 10 times)
10. If nothing received: `_I2CReset()` + retry

## Implicit Overhead Between Transactions

Even without hooks, note-c has significant CPU overhead between the end
of transaction A and the start of transaction B:

**After transaction A completes:**
1. CRC check on response (`_crcError` — string scan + CRC32)
2. Parse response JSON (`JParse` — malloc + tree build)
3. Free serialized request (`_Free(json)`)
4. Free response string (`_Free(rspJsonStr)`)
5. Sequence number increment
6. Unlock Notecard (`_UnlockNote`)
7. Transaction stop hook (`_TransactionStop`)

**User code runs (builds next request)**

**Before transaction B starts:**
1. Create request object (`JCreateObject` + `JAddStringToObject`)
2. Serialize request (`JPrintUnformatted` — malloc + string build)
3. Transaction start hook (`_TransactionStart(30000)`)
4. User agent injection check
5. Timeout calculation (`_noteTransaction_calculateTimeoutMs`)
6. CRC addition (`_crcAdd` — malloc + CRC32)
7. Lock Notecard (`_LockNote`)
8. Reset check (if `resetRequired`, full reset sequence)

On a typical MCU this adds 1-5ms of CPU time between transactions.
note-cpp's streaming path skips all of this: no heap allocation, no
tree parsing, no CRC, no hooks. Transactions go back-to-back with
sub-millisecond gaps.

## TurboIO

note-c has a global `cardTurboIO` flag that skips certain delays:
- Serial RX poll delay: 10ms -> 0ms
- I2C IO delay: 6ms -> 0ms

note-cpp's compile-time equivalent: `StaticSerialPolicy<SerialPolicy::fast()>`
eliminates segment delays, reduces retries and timeouts. But it's a
compile-time choice, not runtime-toggleable.

## Design: Inter-Transaction Timing in the Notecard Object

Inter-transaction timing belongs in the `Notecard` object, not the
transport. The Notecard object represents the peer device; the
transport is just the wire. A single Notecard can have multiple
transports (I2C + aux serial), and processing time is a property of
the Notecard, not the bus.

### Layer responsibilities

| Layer | Responsibility |
|-------|---------------|
| **Transport** (NotecardSerial/I2C) | Wire protocol: segment pacing, framing, intra-transaction timeout, transport-level sync |
| **Notecard** | Inter-transaction timing, per-request timeout, safety-gated retry, last-transaction tracking |

### Inter-transaction timing

The Notecard object tracks when the last transaction completed
(wall-clock). Before starting the next transaction, if insufficient
time has passed, it waits the remaining delta. This is a timeout-based
wait, not a fixed delay — if enough time has naturally passed (user
code, other processing), no wait occurs.

The minimum gap is configurable and defaults to a value that matches
note-c's typical overhead (~5ms or whatever testing shows is needed).
Applications that need more (e.g. 2s after `card.attn` for env
population) can increase it.

### Per-request timeout

The Notecard object calculates the transaction timeout based on request
type, matching note-c's `_noteTransaction_calculateTimeoutMs`:

- Default: 30s
- `note.add` / `web.*` with `seconds` or `milliseconds`: use that + 1s
- Override via `NoteSetRequestTimeout` equivalent

### Passthrough requests

Raw `transact(json)` and `send(json)` cannot inspect the JSON for
request type or safety classification. They must assume:

- **Safety**: `NonIdempotent` (never retry on `ResponseLost`)
- **Timeout**: default (30s)

Callers who know their passthrough request is safe can opt in to retry
via an explicit policy override.

### Hook point (CTX/RTX support)

For Notecard SKUs that support CTX/RTX deep sleep wake, or platforms
that need custom readiness logic (FreeRTOS mutex, power management):

```cpp
notecard.set_transaction_hooks(
    [](uint32_t timeout_ms) -> bool { /* wait for ready */ },
    []() { /* transaction complete */ }
);
```

Without hooks, the Notecard uses its built-in wall-clock timing.

## Implementation Status

**Addressed:**

- **Inter-transaction timing**: `TransactionTiming` in the Notecard
  object. Wall-clock gap enforced before every transaction via
  `set_inter_transaction_gap(ms)`. Default 2ms.
- **Safety-gated retry**: `retry_transaction()` checks `RequestT::safety`
  before retrying. NonIdempotent/Destructive never retry on ResponseLost.
- **Transport single-attempt**: `StreamingTransport` retry loop removed.
  Retry orchestrated by Notecard with full request context.
- **Consistent retry across all paths**: streaming, buffered, passthrough,
  StaticNotecard, BareNotecard all use `retry_transaction()`.
- **Request IDs**: auto-incrementing `"id"` field for log correlation.

**Remaining gaps:**

1. **Per-request timeout calculation**: note-c inspects `note.add` and
   `web.*` for timeout fields. note-cpp uses a single default.

2. **Serial RX poll delay**: 1ms vs 10ms — note-cpp polls 10x faster.
   Low impact (saves power on note-c, minor on note-cpp).

3. **TurboIO runtime toggle**: note-cpp has compile-time fast policy
   but no runtime toggle. note-c's `NoteTurboIO()` is deprecated.

4. **CRC**: note-c adds CRC to requests and validates on responses,
   retrying on mismatch. note-cpp has CRC support but it's not yet
   integrated with the Notecard-level retry loop.
