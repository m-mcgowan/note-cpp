# FAQ

Short answers to questions that come up before you've found the deeper doc. Each entry resolves the immediate "do I need this / does it work / where does this live" decision in two or three sentences and points at the canonical reference for the full picture. If a question you hit isn't answered here, [open an issue](https://github.com/m-mcgowan/note-cpp/issues) — this page is expected to grow.

## Do I need cJSON?

No. Streaming mode parses responses with a SAX pipeline directly into your typed `Response` (or your own struct via `.into(T&)`) — no JSON library linked, no tree built. You only need a `JsonBackend` (cJSON, nlohmann, or `StaticJsonBackend`) when you want `response.body()` to return a walkable tree; see [`json-backend.md`](json-backend.md) for the trade-off.

## Do I need an arena?

No, by default. The `Notecard` constructor with a `JsonBackend` lets fields use the heap, and response strings stay valid until the next `execute()`. Add a `MonotonicArena` when you want zero allocation, or when you need response strings to outlive the next call — `NOTE_MINIMAL` switches AVR to the streaming path that requires one. See [`memory.md`](memory.md#do-i-need-to-do-anything-special).

## Is `note-cpp` Arduino-only?

No. The same typed API compiles on stdcpp, ESP-IDF, Zephyr, and bare-metal — Arduino just bundles a convenience `note::arduino::Notecard` that wires `Serial`/`Wire` for you. On other platforms you build a `Hal` and pick a backend explicitly; the `nc.card.version().execute()` surface is identical. See [`getting-started.md`](getting-started.md#pick-your-platform) for non-Arduino setup.

## Can I use `note-cpp` alongside `note-arduino`?

Yes — incremental migration is supported via a small bridge that routes `note-cpp` requests through `note-arduino`'s existing `NoteRequestResponseJSON` transport. The `Notecard` name lives under `note::Notecard`, so qualifying it (or setting `NOTE_USING_NAMESPACE 0`) avoids collision with note-arduino's global `Notecard`. See [`examples/arduino/note-arduino-bridge/README.md`](../examples/arduino/note-arduino-bridge/README.md) and the [migration guide](platforms/arduino/migration-from-note-arduino.md).

## What's the smallest target this runs on?

ATmega328P (Arduino Uno — 32 KB flash, 2 KB RAM) is the validated floor. With `NOTE_MINIMAL` an 8-endpoint app fits comfortably with room for application code. Smaller hasn't been tried; for the AVR-specific patterns and the live size matrix see [`platforms/arduino/avr-guide.md`](platforms/arduino/avr-guide.md).

## Is there a synchronous-vs-async distinction?

`note-cpp` is synchronous — `execute()` blocks until the Notecard responds, and the round-trip on serial/I2C is fast enough that this is rarely a problem. If you need the host to sleep between events instead of polling, use `card.attn`: the Notecard drives the ATTN pin as an interrupt and your code wakes only when something happens. See [`platforms/arduino/card-attn-guide.md`](platforms/arduino/card-attn-guide.md).

## Where do I report bugs or suggest features?

[GitHub issues on `m-mcgowan/note-cpp`](https://github.com/m-mcgowan/note-cpp/issues). Confirmed library bugs and their workarounds are catalogued in [`known-issues.md`](known-issues.md) — check there first in case the symptom you're seeing already has a workaround.

## See also

- [`getting-started.md`](getting-started.md) — top-down walkthrough from a clean project to your first request
- [`glossary.md`](glossary.md) — Notecard wire vocab and `note-cpp` library vocab side by side
- [`using-the-api.md`](using-the-api.md) — calling styles, focused operations, escape hatches
