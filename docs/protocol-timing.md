# Protocol Timing — note-c vs note-cpp

## Summary

Comparison of timing constraints in the Notecard wire protocol as
implemented by note-c and note-cpp.

## Serial Transport

| Parameter | note-c | note-cpp | Match |
|-----------|--------|----------|-------|
| Segment max length | 250 bytes (`CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN`) | 250 bytes (`segment_max_len`) | ✅ |
| Segment delay | 250ms (`CARD_REQUEST_SERIAL_SEGMENT_DELAY_MS`) | 250ms (`segment_delay_ms`) | ✅ |
| Chunked transmit | Yes — `_serialChunkedTransmit` | Yes — `NotecardSerial::transmit()` chunks at segment_max_len | ✅ |
| RX poll delay (idle) | 10ms (`_DelayMs(10)` when `!cardTurboIO`) | 1ms (`hal_.delay(1)`) | ❌ Faster |
| Intra-transaction timeout | 1s (`CARD_INTRA_TRANSACTION_TIMEOUT_SEC`) | 1s (`intra_timeout_ms`) | ✅ |
| Reset drain window | 500ms (`CARD_RESET_DRAIN_MS`) | 500ms (`reset_drain_ms`) | ✅ |
| Reset sync retries | 10 | 10 (`reset_sync_retries`) | ✅ |
| Max transaction retries | 5 | 5 (`max_retries`) | ✅ |
| Retry delay | 500ms (`RETRY_DELAY_MS`) | 500ms (`retry_delay_ms`) | ✅ |
| Line terminator | `\r\n` | `\r\n` | ✅ |
| `cardTurboIO` (skip IO delays) | Yes — global flag | No equivalent | ❌ Missing |

## I2C Transport

| Parameter | note-c | note-cpp | Match |
|-----------|--------|----------|-------|
| Segment max length | 250 bytes (`CARD_REQUEST_I2C_SEGMENT_MAX_LEN`) | 250 bytes (`segment_max_len`) | ✅ |
| Segment delay | 250ms (`CARD_REQUEST_I2C_SEGMENT_DELAY_MS`) | 250ms (`segment_delay_ms`) | ✅ |
| IO delay (pre-operation) | 6ms (`_delayIO`, `!cardTurboIO`) | 6ms (`io_delay_ms`) | ✅ |
| Chunk delay (inter-chunk) | 20ms (`CARD_REQUEST_I2C_CHUNK_DELAY_MS`) | 20ms (`chunk_delay_ms`) | ✅ |
| NACK wait | 1000ms (`CARD_REQUEST_I2C_NACK_WAIT_MS`) | 1000ms (`nack_wait_ms`) | ✅ |
| Response poll interval | 50ms | 50ms (`response_poll_ms`) | ✅ |
| Intra-transaction timeout | 1s | 1s (`intra_timeout_ms`) | ✅ |

## Request-Level Timing

| Parameter | note-c | note-cpp | Match |
|-----------|--------|----------|-------|
| Inter-transaction timeout | 30s (`CARD_INTER_TRANSACTION_TIMEOUT_SEC`) | Not implemented | ❌ Missing |
| Transaction lock (`_TransactionStart`) | Yes — hook-based, waits for Notecard ready | Not implemented | ❌ Missing |
| Transaction unlock (`_TransactionStop`) | Yes — hook-based | Not implemented | ❌ Missing |
| Per-request timeout override | Yes — `NoteSetTransactionTimeout` | Not implemented | ❌ Missing |
| Suppress debug during transaction | Yes — `suppressShowTransactions` | Not applicable (debug is per-listener) | N/A |

## What note-c's Inter-Transaction Timeout Does

Before every request, note-c calls `_TransactionStart(30000)`:
1. Invokes `hookTransactionStart(timeoutMs)` if set
2. The hook typically checks if the Notecard is ready (e.g., not busy
   processing a previous request)
3. Returns `true` when ready, `false` on timeout
4. If `false`, the request is aborted with "Notecard not ready"

After every request, note-c calls `_TransactionStop()`:
1. Invokes `hookTransactionStop()` if set
2. Signals that the transport is free for the next request

This ensures serialized access to the Notecard on multi-threaded
platforms (ESP32 FreeRTOS tasks).

## What note-cpp Doesn't Have

### cardTurboIO

note-c has a global `cardTurboIO` flag that skips certain delays:
- Serial RX poll delay (10ms → 0ms)
- I2C IO delay (6ms → 0ms)

note-cpp's equivalent: use the `fast()` policy:
```cpp
NotecardSerial<StaticSerialPolicy<SerialPolicy::fast()>> transport(hal);
```

This eliminates segment delays, reduces retries and timeouts. But it's
a compile-time choice, not runtime-toggleable like `cardTurboIO`.

### Inter-Transaction Timeout / Transaction Lock

note-cpp has no equivalent of `_TransactionStart`/`_TransactionStop`.
This means:
- No "Notecard ready" check before sending
- No mutex for multi-threaded access
- No configurable per-request timeout

Impact: on ESP32 with multiple FreeRTOS tasks accessing the Notecard,
concurrent requests can corrupt the protocol. On single-threaded
Arduino, this is not an issue.

## Gaps to Address

1. **Serial RX poll delay**: 1ms vs 10ms — note-cpp polls 10x faster.
   Low impact (saves power on note-c, minor on note-cpp).

2. **cardTurboIO runtime toggle**: note-cpp has compile-time fast policy
   but no runtime toggle. Could add a `set_turbo(bool)` method that
   adjusts the runtime policy fields.

3. **Inter-transaction timeout**: needed for ESP32 multi-task safety.
   Design: add `set_transaction_hooks(start_fn, stop_fn)` on Notecard,
   called before/after each `execute()` and `transact()`.

4. **Per-request timeout override**: note-c allows overriding the 30s
   default for specific requests (e.g., DFU operations that take longer).
   Could be a parameter on `execute()` or a method on the request type.
