# Handoff — `tests/test_notecard_streaming.cpp`

## Status

The file is registered in `cmake/note-cpp-sources.cmake` (under
`NOTE_CPP_TEST_SOURCES_FULL_ONLY`) and all cases compile on the
default and `NOTE_MINIMAL` builds.

**Four of the five originally-broken cases are now fixed.** One remains
tagged `[!mayfail]`; see below for the suspected library bug.

## Fixed

- **`Streaming: execute without allocator or backend returns NotReady`**
  Harness now calls `nc.clear_allocator()` — the streaming constructor
  defaults `alloc_` to a default-constructed `Allocator`, so
  `has_value()` was true and the NotReady fallback never fired.
- **`Streaming: void execute without allocator returns NotReady`**
  Same fix as above (shared harness).
- **`Streaming: enforce_timing delays when gap is insufficient`**
  Rewritten to use `nc.send(raw_json)` instead of `nc.command(string_view)`.
  The `command(string_view, ...)` overload is a fire-and-forget path that
  intentionally skips `enforce_timing()` / `record_timing()` (unlike
  `command_typed()`, `send_command()`, `execute()`, and `send()`, which
  all honour timing). Using `send()` exercises the same gap logic
  through a path that actually records and enforces timing.
- **`Streaming: record_timing uses streaming millis`**
  Same fix as above.

## Remaining `[!mayfail]`

### `Streaming: binary PUT with verify does pre-flight and post-transmit`

**Assertion:** With `CardBinaryPut.data(...).verify()` and the canned
four-response sequence (reset → pre-flight status → PUT handshake →
post-verify status), `execute()` returns a success result.

**Now:** `rsp.has_value()` is false — error is
`Error::ResponseLost` / `Cause::Unspecified`, message *"binary verify
query failed"*.

**Interesting:** the **buffered-path** equivalent at
`tests/test_binary_execute.cpp:442` (*"Binary PUT: verify() does
pre-flight and post-transmit checks"*) passes with the **same
four-response sequence**. Only the streaming path fails.

**Diagnostic observations (from a one-off `std::cerr` session, since
reverted):**
- All four queued responses are consumed — `rx` is empty at the point
  of failure.
- The expected MD5 string is correct and the verify response is valid
  JSON (`{"status":"5289df737df57326fcdd22597afb1fac"}`).
- The error comes from `binary_control(R"({"req":"card.binary"})")`
  returning `!ok` — i.e. `streaming_transact_raw` itself fails, not the
  MD5 comparison.

**Suspected library bug:** after `binary_write(COBS blocks)` +
`binary_write(EOP)`, the next `streaming_transact_raw` on the same
transport fails somewhere inside `read_line()` before producing a
usable response. The buffered path works because its control queries
go via `transport_->transact()`, not via the streaming byte-level
read path.

**Where to start:** step through `StreamingTransport::read_line` after
a binary-write sequence on `MockStreamHal`. Candidate causes:
- `initialized_` / frame-sync state not reset between the binary
  streaming phase and the follow-up control transaction.
- A byte consumed or skipped by the binary path that `read_line`
  needed.
- `drain_frame_boundary()` firing at the wrong time.

Once fixed:
1. Remove the `[!mayfail]` tag from the `TEST_CASE(...)` line.
2. Remove the leading handoff comment.
3. Delete this file once no `[!mayfail]` tests remain.
