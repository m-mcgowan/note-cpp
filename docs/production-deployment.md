# Production deployment

Shipping a Notecard-equipped device into the field shifts the priorities. The library defaults are tuned for development — debug callbacks armed, retries generous, buffered JSON tree available so `response.body()` works in a debugger. Production wants the opposite: smaller binary, no listener overhead, predictable RAM, and clear hooks for the host's watchdog and field-debug story. This page collects the production-specific knobs and patterns in one place and links to the canonical docs for depth.

## Compile-time flags

The flags below are the production-relevant subset of [`feature-flags.md`](feature-flags.md) — the canonical reference covers the full matrix and per-flag savings.

| Flag | Recommendation | Why |
|------|----------------|-----|
| `NOTE_DEBUG_ENABLED=0` | **Off** in shipped builds | Strips debug listeners, eliminates the `DebugListener` field and all wire/timing/memory call sites. ~500 B flash, ~16 B RAM. Use the compile-time `note::Notecard<note::NoDebug>` policy for the same effect with stricter type guarantees. |
| `NOTE_MINIMAL` | **On** for AVR / Cortex-M0 | Bundle: `NOTE_JSONB=1` (binary wire), no JSON tree backend, no retry, no request IDs, no extras. Halves typical RAM and removes ~3 KB flash. See [feature-flags § `NOTE_MINIMAL`](feature-flags.md#flash-size-reduction-with-note_minimal). |
| `NOTE_NO_RETRY` | On if your app does request-level retry | The library's safety-gated retry is correct by default. Disable only if you've moved retry to the application layer (e.g. queueing requests across reset boundaries). |
| `NOTE_API_VERSION` + `NOTE_API_STRICT` | On if the deployed Notecard firmware is pinned | Removes fields newer than the target firmware via `#if`, so a typo can't reach for a field the deployed device doesn't understand. See [feature-flags § API version gating](feature-flags.md#api-version-gating-and-strict-mode). |
| `NOTE_STRICT_BODY_FIELDS` | **On** (default `1`) — leave it on | Turns silent field-type mismatches in body schemas into compile errors. If your `Reading` struct gains a `std::variant` field the library doesn't serialise, you find out at build time, not in the field. |

The `NOTE_PRINTABLE=0` and `NOTE_EXTRAS=0` flags from feature-flags.md are also worth considering — both shed flash with no behavioural cost if your code doesn't depend on them.

## OTA / firmware updates

`note-cpp` is OTA-agnostic. Notecard firmware updates are driven by the Notecard itself (Notehub schedules a download, the Notecard fetches it over cellular/Wi-Fi) and host firmware updates via [Notecard Outboard Firmware Update](https://dev.blues.io/notecard/notecard-walkthrough/host-mcu-firmware-updates) are driven by the Notecard pulling the host's MCU into a programming mode over the AUX pins. The library's role is small and bounded:

- `nc.card.dfu()` configures the host MCU class and enables/disables Outboard DFU. See [`api-reference.md` § card.dfu](api-reference.md#carddfu).
- `nc.hub.sync()` (or `nc.command("hub.sync")` for fire-and-forget) tells the Notecard to connect now, which is when it picks up scheduled firmware bundles.
- The `.stop`/`.start` fields on `card.dfu` disable / re-enable the host RESET that the Notecard performs as part of the Outboard update sequence — useful when the application is mid-transaction and needs to defer a reset.

The OTA reset window — when the Notecard pulls the host's RESET line — is straddled by a host reboot, so any host-side state held only in RAM is lost across the boundary. Persist whatever has to survive (e.g. queued data, retry counters) to a Notefile or to non-volatile storage before the device commits to an Outboard update window. Platform-specific update orchestration (image staging, signing, rollback) lives in your platform's firmware, not in `note-cpp`.

## Watchdog patterns

Watchdog management is the host's responsibility. `note-cpp` doesn't pet a watchdog from inside `execute()` — a typed `execute()` is short on serial/I2C and rarely warrants per-call petting, but a long [binary transfer](binary-transfer.md) or a stuck transport can block long enough to matter. Two patterns:

- **Host-side software watchdog.** Pet from the application loop around each `execute()`. The library's [timing events](debugging.md#timing-events) (`TransactionBegin`, `TransmitEnd`, `ReceiveBegin`, `TransactionEnd`) give precise hooks if you want to pet from inside the call rather than around it — wire one to `on_timing` and pet on every event.
- **Notecard-driven hardware watchdog.** `nc.card.attn().watchdog().seconds(N)` arms the ATTN pin as a watchdog: the host must call back to the Notecard before `N` seconds elapse or the Notecard pulses ATTN (or RESET, depending on wiring) to reboot the host. Useful when the host can lock up in a state where its own software watchdog is also wedged. See [`platforms/arduino/card-attn-guide.md`](platforms/arduino/card-attn-guide.md).

Pick a window that comfortably exceeds your worst-case `execute()` latency — typed requests are sub-second on serial/I2C, but binary transfers, `hub.sync` runs that traverse cellular, and retries on a flaky link can each push past 30 seconds. Watchdog timeouts that race the slow path produce reset loops in the field that don't reproduce on the bench.

## Log routing

Production debug output rarely goes to a serial console — there isn't one, or it's expensive to leave attached. The [debug listener](debugging.md#custom-listeners) routes wire bytes, timing, transport events, and memory events into a callback. In production, route to a bounded buffer (a ring buffer in RAM, or a small Notefile) and dump on error rather than streaming everything:

```cpp
note::DebugListener d;
d.ctx = &log_ring;
d.on_wire = [](const note::WireEvent& ev, void* ctx) {
    static_cast<LogRing*>(ctx)->push(ev.json);   // bounded, lossy on overflow
};
d.on_transport = [](note::TransportEvent ev, int detail, void* ctx) {
    static_cast<LogRing*>(ctx)->dump_to_notefile();  // flush on any transport hiccup
};
nc.set_debug(d);
```

The transport-event channel (`Retry`, `CrcMismatch`, `Timeout`, `SendFailed`) is the right trigger for a flush — those are the events you want post-mortem context for. Build with `NOTE_DEBUG_ENABLED=1` even in production if you want this; the listener costs ~16 B RAM when armed and the call sites are inlined null-checks when no listener is set. See [`debugging.md`](debugging.md).

## Recovery from unexpected resets

The Notecard's session state survives host resets — request IDs, queued Notes, environment variables, and pending sync state are all on the Notecard, not the host. After a host reboot:

- `nc.begin()` is idempotent on the host side. Re-binding the transport and re-issuing the first request is enough; you don't need to "log back in."
- CRC sequence numbers reset on the host but the Notecard accepts the new sequence on the next request. If you see `send_failed[timeout]` immediately after a reset, see [`troubleshooting.md` § `send_failed[timeout]`](troubleshooting.md#im-seeing-send_failedtimeout-after-it-was-working) — `nc.reset()` cycles the transport and clears stale CRC state.
- A `card.attn` arm is **not** guaranteed to persist across Notecard resets. If the host expects ATTN-driven wake, re-arm during host startup (the call is cheap, and `.rearm()` is idempotent — see [`card-attn-guide.md` § arm vs rearm](platforms/arduino/card-attn-guide.md#arm-vs-rearm)).
- In-flight binary transfers do not survive a reset on either side; the verify step on the next attempt will detect the mismatch and the transfer can be retried from scratch. See [`binary-transfer.md`](binary-transfer.md).

If your host reset was caused by an OOM or a stack overflow, the failure is upstream of `note-cpp` — the library uses zero heap by default in sink mode and ~800 B static RAM. See [`memory.md`](memory.md) for sizing guidance. If resets are correlated with cellular sync attempts, the cause is more likely brown-out from the cellular radio's TX peak current than anything in the library — check your power supply margin.

## What to leave on / off in production

A quick-decision aid. The full story for each flag is in [`feature-flags.md`](feature-flags.md).

| Concern | Recommendation | Notes |
|---------|----------------|-------|
| Debug printing | **Off** (`NOTE_DEBUG_ENABLED=0` or `note::NoDebug` policy) | Or leave on with a bounded log-ring listener — your call. |
| Tree-mode JSON backend | **Off** unless you actually call `response.body()` | Sink mode (no `JsonBackend`) is the lowest-memory path; typed responses and `req.into(T&)` work the same way. |
| Heap | **Off** for embedded | Default behaviour in sink mode — zero allocation. See [`memory.md`](memory.md). |
| JSONB binary wire | **On** if size matters | Auto-enabled by `NOTE_MINIMAL`. Requires Notecard firmware 11.x+. |
| Retry | **On** unless your app retries at the request level | The library's retry handles transient transport faults; replacing it is rarely worth the complexity. |
| Strict body fields | **On** (default) | Catches schema drift at compile time. |
| Request IDs | On in production | Cheap (~5 B RAM) and the wire log is much easier to read with them. |

## See also

- [`feature-flags.md`](feature-flags.md) — the canonical flag reference, with savings figures
- [`debugging.md`](debugging.md) — debug categories, custom listeners, timing/transport events
- [`troubleshooting.md`](troubleshooting.md) — symptom → cause → fix catalog for common production-time failures
- [`known-issues.md`](known-issues.md) — confirmed library bugs with workarounds
- [`memory.md`](memory.md) — zero-allocation patterns and arena sizing
- [`platforms/arduino/card-attn-guide.md`](platforms/arduino/card-attn-guide.md) — ATTN pin configuration, including the `watchdog` mode
