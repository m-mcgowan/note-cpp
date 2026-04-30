# Transport Retry and Request Importance Design

## Background

When communicating with the Notecard over serial or I2C, requests can fail
at several points. The transport may fail to send, the response may be lost,
or the response may arrive corrupted. The library needs a retry strategy that
balances reliability with safety — retrying a `note.add` that may have already
been processed risks duplicating data.

note-c retries all requests blindly up to 5 times on I/O errors. This is
simple but unsafe for non-idempotent requests like `note.add` — if the
request was processed but the response was lost, the retry creates a
duplicate note.

## Error Phases

| Phase | What happened | Can retry? |
|-------|---------------|------------|
| **SendFailed** | Request never reached the Notecard | Always safe |
| **ResponseLost** | Request sent, response not received (timeout, CRC) | Depends on request safety |
| **Notecard** | Valid response with `{"err":"..."}` | Generally no (application error) |

## Request Safety

Every generated request type carries a compile-time `Safety` classification:

| Safety | Meaning | Examples |
|--------|---------|----------|
| `ReadOnly` | No side effects | `card.version`, `card.temp` read |
| `Idempotent` | Repeating has same effect | `hub.set`, `env.default` |
| `NonIdempotent` | Repeating may cause duplicates | `note.add` |
| `Destructive` | Repeating may lose data | `note.get` pop |

## Retry Safety Matrix

| Error | ReadOnly | Idempotent | NonIdempotent | Destructive |
|-------|----------|------------|---------------|-------------|
| `SendFailed` | Retry | Retry | Retry | Retry |
| `ResponseLost` | Retry | Retry | **No retry** | **No retry** |
| `Notecard` (transient) | Retry | Retry | Maybe | No |
| `Notecard` (permanent) | No | No | No | No |

### Future: note-c issue #238

If the Notecard implements last-response caching (issue #238), the host could
safely re-send a non-idempotent request with the same sequence ID and receive
the cached response without re-executing the request.

**Detection:** The Notecard would need to advertise support for this feature
(e.g. a capability flag in `card.version` response, or a specific behavior
when a duplicate sequence ID is received). Until this is detectable, note-cpp
should NOT retry non-idempotent requests on `ResponseLost`.

**Transition plan:**
1. Today: no retry on `ResponseLost` for `NonIdempotent`/`Destructive`
2. When #238 lands: detect via capability flag, enable safe re-send with
   sequence ID for `NonIdempotent` (still not for `Destructive`)

## Request Importance

Rather than separate `.command()` and `.execute()` methods, a unified dispatch
method uses an importance level that determines both the delivery mode and
retry behavior:

| Importance | Delivery | Expects response | Retry effort |
|------------|----------|-----------------|--------------|
| `BestEffort` | Command (`cmd`) | No | None |
| `Casual` | Request (`req`) | Yes | 1 retry, short timeout |
| `Normal` | Request (`req`) | Yes | Default policy (5 retries) |
| `Important` | Request (`req`) | Yes | Extended retries, longer timeout |
| `Critical` | Request (`req`) | Yes | Maximum retries, longest timeout |

```cpp
enum class Importance : uint8_t {
    BestEffort,   // fire-and-forget (command mode)
    Casual,       // try once or twice
    Normal,       // default retry policy
    Important,    // try harder (e.g. hub.set during setup)
    Critical,     // must succeed (e.g. connection management)
};
```

### API

```cpp
// Unified dispatch — replaces both .execute() and .command()
auto result = nc.hub.set()
    .product("com.example.app")
    .mode("periodic")
    .send(Importance::Important);

// .execute() and .command() remain as convenience aliases:
//   .execute()  → .send(Importance::Normal)
//   .command()  → .send(Importance::BestEffort)

// Per-request policy override
auto result = nc.note.add()
    .file("sensors.qo")
    .body(r)
    .send(Importance::Normal, RetryPolicy{.max_retries = 0});  // no retries
```

### RetryPolicy

```cpp
struct RetryPolicy {
    uint8_t max_retries = 5;
    uint16_t retry_delay_ms = 500;
    uint32_t timeout_ms = 30000;       // overall timeout (0 = no limit)
    bool unsafe_retry = false;          // if true, retry even when Safety says not to
                                        // Use only when the caller has external dedup
                                        // (e.g. Notehub route dedup). Queued notes
                                        // (.qo files) have no keying/dedup mechanism.
};
```

Each importance level maps to a default `RetryPolicy`:

| Importance | max_retries | retry_delay_ms | timeout_ms |
|------------|-------------|----------------|------------|
| `BestEffort` | 0 | — | — |
| `Casual` | 1 | 250 | 5000 |
| `Normal` | 5 | 500 | 30000 |
| `Important` | 10 | 500 | 60000 |
| `Critical` | 20 | 1000 | 120000 |

Per-request overrides use a `RetryPolicy` struct where unset fields (using
sentinel values) inherit from the importance-derived defaults.

### Interaction with request Safety

The retry loop checks both the importance-derived policy AND the request's
`Safety` classification:

```
if (error == SendFailed):
    → retry per policy
elif (error == ResponseLost):
    if (!policy.unsafe_retry && !is_safe_to_retry(Request::safety)):
        → return error (do not retry)
    else:
        → retry per policy
elif (error == Notecard):
    if (is_transient_notecard_error(err.message)):
        → retry per policy (with backoff)
    else:
        → return error (permanent)
```

### Transient vs permanent Notecard errors

Some Notecard error strings indicate transient conditions that may resolve:
- `"queue full"` — buffer temporarily exhausted
- `"busy"` — Notecard processing another request

Others are permanent:
- `"bad request"` — malformed request
- `"not supported"` — endpoint doesn't exist on this firmware

The classification could be driven by the OpenAPI spec (error categories per
endpoint) or by pattern matching on the error string. Initially, only I/O
errors and explicit transient strings trigger retries.

## Testing

### Unit tests (mocks)
- Verify retry count for each error phase × safety combination
- Verify timeout is respected
- Verify per-request policy overrides work
- Verify importance → policy mapping

### Fuzz testing (mocks)
- Random transport failures (drop bytes, corrupt, timeout)
- Verify non-idempotent requests are never re-sent on ResponseLost
- Verify retry count never exceeds policy

### Integration tests (hardware)
- Verify retry succeeds after transient I2C/serial failures
- Verify note.add is not duplicated on timeout
- Verify hub.set succeeds after transient failure

## Transport Interface

The current function-based transport (`RequestFn`/`SendFn`) needs to become
a proper object to support reset and abort:

```cpp
struct ITransact {
    virtual ~ITransact() = default;

    /// Send a request and receive the response.
    /// Returns string_view into transport's internal buffer.
    virtual Result<string_view> transact(string_view request, uint32_t timeout_ms) = 0;

    /// Send a command (fire-and-forget, no response expected).
    virtual Result<void> send(string_view request) = 0;

    /// Reset the transport to a known state (flush buffers, re-sync framing).
    /// Called after I/O errors before retrying (matches note-c's _Reset()).
    virtual void reset() = 0;

    /// Abort an in-progress transaction.
    /// Sets a flag that the transport's polling loop checks, causing it to
    /// return promptly with an error. Called when the retry timeout expires
    /// during a transaction.
    virtual void abort() = 0;
};
```

`SerialFramer` and `I2cFramer` already implement most of this as
duck-typed callables. The refactor formalizes them as `ITransact`
implementations. `Notecard` takes `ITransact&` instead of `RequestFn`/`SendFn`.

The retry loop calls `reset()` after each failed attempt (matching note-c)
and can call `abort()` to interrupt a transport that's blocked mid-receive
when the overall retry timeout expires.

## Timeout Semantics

Two timeouts are in play:

- **Transport timeout** (`transact()` timeout_ms parameter): how long the
  transport waits for a single response. If the Notecard stalls mid-response,
  the transport returns `ResponseLost` + `Cause::TimeoutIntra`.

- **Retry timeout** (`RetryPolicy::timeout_ms`): total wall-clock budget for
  all attempts, including retry delays. Checked between attempts. If exceeded,
  no further retries are attempted and the last error is returned.

  The transport timeout for later retry attempts is shortened to fit within
  the remaining retry budget.

```
Timeline example:
  t=0s      Attempt 1 → request sent, Notecard stalls mid-response
  t=10s     Transport timeout fires → ResponseLost
  t=10s     reset() called → transport re-initialized
  t=10.5s   Retry delay (500ms)
  t=10.5s   Check: 10.5s < 30s → retry allowed
  t=10.5s   Attempt 2 → transport timeout set to min(10s, 30s - 10.5s) = 10s
  ...
  t=30s     Retry timeout exceeded → return last error
```

## Importance-Level Policy Configuration

Default policies per importance level are a static table. Customizable
globally or per-level:

```cpp
nc.set_importance_policy(Importance::Critical, {
    .max_retries = 30,
    .retry_delay_ms = 2000,
    .timeout_ms = 300000,
});
```

## Per-Request Overrides

Overrides use sentinel values (no `std::optional` overhead on embedded):

```cpp
struct RetryPolicyOverride {
    static constexpr auto inherit = /* sentinel */;
    uint8_t max_retries = inherit;
    uint16_t retry_delay_ms = inherit;
    uint32_t timeout_ms = inherit;
    int8_t unsafe_retry = -1;  // -1 = inherit, 0 = false, 1 = true
};

// Override only what you need:
nc.hub.set().product("...").send(Importance::Normal, {.timeout_ms = 60000});
```

The resolved policy starts from the importance level's defaults, then
overlays any non-sentinel override values.

## Implementation Status

### Done

- **Transport simplification**: `Protocol` is now single-attempt.
  Retry moved to the Notecard layer. Constructor's `max_retries`/`retry_delay_ms`
  params are deprecated (accepted but ignored).
- **`RetryPolicy`** (`include/note/retry_policy.hpp`): `max_retries` (default 5),
  `retry_delay_ms` (500), `timeout_ms` (30000).
- **`TransactionTiming`** (`include/note/retry.hpp`): wall-clock gap enforcement.
  `min_gap_ms` (default 2ms, configurable via `set_inter_transaction_gap()`).
- **`retry_transaction()`** (`include/note/retry.hpp`): free function template
  used by Notecard, StaticNotecard, and BareNotecard. Enforces inter-transaction
  gap, gates retry on `Safety`, respects timeout budget.
- **Notecard integration**: `execute()` wrapped with retry using `RequestT::safety`.
  `transact()` wrapped with `Safety::NonIdempotent`. `send()`/`command_typed()`
  get timing enforcement but no retry.
- **Request IDs**: auto-incrementing `"id"` field injected into JSON requests
  for log correlation. Configurable via `set_request_ids(bool)`.
- **`millis()` on Hal**: required for timing. All HALs implement it.
- **StaticNotecard and BareNotecard**: same retry/timing as Notecard.
  BareNotecard has explicit `Safety` overload for callers who know their
  passthrough request is safe.
- **Error classification fix**: SAX parse failures on the wire are now
  `Error::ResponseLost` (not `Error::Json`) — they're retryable for safe requests.
- **Unit tests**: 17 tests in `test_retry.cpp` covering Safety × Error matrix,
  timeout budget, gap enforcement, reset between retries, end-time recording.

### Future

- `Importance` enum and per-importance policy tables (deferred — `RetryPolicy`
  is the foundation, `Importance` is API sugar on top)
- `.send(Importance, RetryPolicyOverride)` on request types
- `set_importance_policy()` global customization
- Fuzz tests with random transport failures
- note-c issue #238 last-response caching (safe retry for NonIdempotent)
