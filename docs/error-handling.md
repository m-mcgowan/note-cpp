# Error Handling

Every `note-cpp` operation returns a result type that is truthy on success. On failure, `error()` gives you a structured `ErrorInfo` with a phase-based error code, an optional diagnostic cause, and a human-readable message.

## Result types

| Type | Used by | Success access |
|------|---------|----------------|
| `Result<T>` | Transport, low-level ops | `*result` or `result.value()` |
| `ApiResult<Response>` | Typed API requests | Dot-access fields directly on result |

```cpp
// ApiResult — fields are accessed directly (no dereferencing)
auto result = api.card.version().execute();
if (result) {
    auto version = result.version;   // string_view
    auto device  = result.device;    // string_view
}

// Result<T> — dereference to get the value
Result<std::string> r = nc.request("card.version");
if (r) {
    std::string json = *r;
}
```

## ErrorInfo

```cpp
struct ErrorInfo {
    Error code;           // what happened (phase-based)
    Cause cause{};        // why it happened (diagnostic)
    std::string_view message;  // human-readable detail
};
```

### Error — what happened

`Error` is a phase-based enum that tells you both what went wrong and whether retrying is safe:

| Error | Meaning | Retry safe? |
|-------|---------|-------------|
| `SendFailed` | Request never reached the Notecard | Always |
| `ResponseLost` | Notecard may have processed the request, but we lost the response | Only if request is `ReadOnly` or `Idempotent` |
| `Notecard` | Notecard returned a valid response with `{"err":"..."}` | Depends on the error |
| `Json` | Local JSON build or parse failure | No (fix the code) |
| `NotReady` | HAL reset failed, transport not usable | After re-init |
| `Overflow` | Buffer too small | No (increase buffer) |
| `InvalidArg` | Bad caller input | No (fix the code) |

### Cause — why it happened

`Cause` provides diagnostic detail. Most callers can ignore it; callers that want adaptive behavior (back-off on repeated timeouts, bus reset on NACK) can switch on it:

| Cause | Meaning |
|-------|---------|
| `Unspecified` | No additional context |
| `Timeout` | No response within the request deadline |
| `TimeoutIntra` | Response started arriving but stalled (inter-byte timeout) |
| `HalError` | I2C NACK or HAL transmit/receive returned false |
| `CrcMismatch` | Response CRC validation failed |

## Formatting

`to_string(ErrorInfo)` produces a human-readable string for logging:

```cpp
ErrorInfo err = result.error();
std::string s = to_string(err);
// "response_lost[timeout]: no response within deadline"
// "notecard: {some device has no ProductUID configured}"
// "send_failed[hal_error]: I2C write returned false"
```

When `cause` is `Unspecified`, the bracket is omitted: `"notecard: {message}"`.

## Safety levels and retry

Each generated request type carries a compile-time safety level:

| Safety | Meaning | Examples |
|--------|---------|----------|
| `ReadOnly` | Pure query, no side effects | `card.version`, `note.get` (read) |
| `Idempotent` | Can be repeated without harm | `hub.set`, `note.template` |
| `NonIdempotent` | Repeating may cause duplicates | `note.add` |
| `Destructive` | Repeating causes data loss | `note.get` (delete/pop) |

Your transport or retry logic can inspect `Request::safety` to decide whether to retry on `ResponseLost`:

```cpp
template<typename Req>
auto retry_once(Notecard& nc, Req req) -> ApiResult<typename Req::Response> {
    auto r = req.execute(nc);
    if (!r && r.error().code == Error::ResponseLost) {
        if constexpr (Req::safety <= Safety::Idempotent) {
            return req.execute(nc);  // safe to retry
        }
    }
    return r;
}
```

## Creating errors

Helper functions for transport implementors:

```cpp
// Simple error
return make_error(Error::SendFailed, "I2C write failed");

// Error with cause
return make_error(Error::ResponseLost, Cause::Timeout, "no response within 5000ms");
```
