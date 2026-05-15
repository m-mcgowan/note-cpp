# Troubleshooting

This page is "I tried something and it didn't work." Each entry pairs a symptom (phrased the way you'd describe it) with the likely cause and the fix, then points at the deeper doc when there is one. For the narrower catalog of confirmed library bugs and their workarounds, see [`known-issues.md`](known-issues.md). For a top-down introduction, see [`getting-started.md`](getting-started.md).

## I'm getting no response from the Notecard

`nc.card.version().execute()` returns falsy and nothing visible happens on the wire. Three causes account for almost all of these:

- **Wrong serial pins or baud.** On Arduino, `Serial1` defaults differ across cores — pioarduino's ESP32 `Serial1` uses pins that aren't broken out on every devkit. Wire a USB-serial adapter to the same TX/RX pins and run a baud sniffer to confirm bytes are leaving the host.
- **I2C wiring incomplete.** The Notecard does not pull SDA/SCL high on its own — see [§ My I2C transactions hang](#my-i2c-transactions-hang) below.
- **Notecard not powered or in deep sleep.** A Notecard that lost power mid-session won't ack. Cycle `V+`/`GND` and retry.

For a fast, low-overhead "is the Notecard reachable at all?" check, call `nc.ping()`. It sends a single `echo` request with a 16-character random nonce and confirms the same nonce comes back. There is no retry, no CRC, and no transport reset on failure, so the call returns quickly (default timeout 500 ms) and tells you whether the link is alive without disturbing any in-flight state. A truthy `ping()` paired with a still-failing application call points the investigation at the application surface; a falsy `ping()` points at the link itself, and you can fall through to wire tracing.

Enable wire tracing (`NOTE_DEBUG_ENABLED=1` plus a debug listener — see [`debugging.md`](debugging.md)) and watch the bytes; if nothing leaves the host the cause is local, if bytes leave but no response arrives the cause is wiring or power.

## My response is empty, or fields are zero-length

`if (r)` succeeded but `r.some_field` is `0` or an empty `string_view`. Cause: the Notecard didn't include that field in the response, and `note-cpp` parses absent fields as default-constructed (`0`, empty `string_view`). Use `has_value()` to distinguish absent from zero:

```cpp
if (r) {
    if (r.time.has_value()) {
        Serial.println(r.time);          // present, even if 0
    } else {
        Serial.println("no time field");
    }
}
```

See [`working-with-responses.md` § Checking for fields](working-with-responses.md#checking-for-fields).

## My body struct isn't being parsed

`req.into(my_struct).execute()` succeeds, but `my_struct`'s fields are still zero. Cause: the struct can't be reflected. On C++17 you must add `NOTE_FIELDS(...)`; on C++20 most plain aggregates work automatically — but a struct with a user-defined constructor stops being an aggregate and falls back to needing the macro:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)   // required on C++17, and on C++20 non-aggregates
};
```

See [`body-values.md` § NOTE_FIELDS macro](body-values.md#note_fields-macro).

## `req.names({...})` doesn't chain into `.execute()`

`auto r = nc.env.get().names({"a", "b"}).execute();` won't compile, or compiles to something that looks wrong. Cause: `names` is an `ArrayField`, and `operator()` on it returns the array, not the request — there's no chainable setter. Set it via assignment, then continue the chain on the request:

```cpp
auto req = nc.env.get();
req.names = {"a", "b"};                  // operator= on ArrayField
auto r = req.into(cfg).execute();
```

See [`environment-variables.md` § Read multiple variables into a struct](environment-variables.md#2-read-multiple-variables-into-a-struct).

## ESP32: `Serial1` prints garbage or sends nothing

Cause: pioarduino's `Serial1` default RX/TX pins differ from stock espressif32, and on some boards those pins aren't broken out at all. Fix: call `Serial1.begin(9600, SERIAL_8N1, rx, tx)` with explicit pins *before* handing `Serial1` to `nc.begin()`, or use a different UART:

```cpp
void setup() {
    Serial1.begin(9600, SERIAL_8N1, /*rx=*/16, /*tx=*/17);
    nc.begin(Serial1, 9600);
}
```

See the [Arduino guide § Setup](platforms/arduino/guide.md#setup). The pioarduino platform also defaults to `gnu++11`; if you want C++20 features, [`cpp-version-compatibility.md` § Setting the standard in your build](cpp-version-compatibility.md#setting-the-standard-in-your-build) covers the build flags.

## I'm seeing `send_failed[timeout]` after it was working

Cause is usually one of:

- **CRC mismatch after auto-detection.** The Notecard turns CRC on as soon as it sees the host include one. If the host then sends a request without a fresh sequence number (because firmware reset between requests, or because the transport was rebuilt mid-session), the Notecard rejects it and the host blocks until timeout.
- **Baud-rate mismatch on serial.** The Notecard's auto-baud only locks within a window after its own reset; a host reset that lands outside that window leaves the two sides talking past each other.

Fix: call `nc.reset()` to cycle the transport and clear CRC state, or destroy and rebuild the `Notecard` object. See [`transport-crc.md` § Auto-detection](transport-crc.md#auto-detection).

## My AVR build is overflowing flash

Cause: the typed API plus a tree-mode JSON backend is roughly 25 KB on AVR; an Uno (ATmega328P) has 32 KB total. Without the right flags an 8-endpoint app spills over before it leaves room for application code. Fix: define `NOTE_MINIMAL`, which bundles JSONB on, streaming mode (no JSON backend), no retry, and no request IDs:

```ini
; platformio.ini
build_flags = -DNOTE_MINIMAL
```

If that's still tight, drop to a smaller API style. The four measured styles on AVR range from 11 KB (raw + `JsonView`) up to 25 KB (typed API groups), all driving the same 8-endpoint reference app. See [`platforms/arduino/avr-guide.md` § Choose your API style](platforms/arduino/avr-guide.md#choose-your-api-style) for the matrix and [`feature-flags.md`](feature-flags.md) for the per-flag savings breakdown.

## My I2C transactions hang

Cause: the I2C bus needs pull-up resistors on both SDA and SCL, and the Notecard does not drive them. On a devkit the host board's onboard pull-ups may be enough; on a custom PCB or a long-wired test rig they usually aren't. Fix: 4.7 kΩ pull-ups to 3V3 on SDA and SCL (10 kΩ also works on shorter buses). If the bus is shared with other drivers, also confirm none of them is holding SDA low after a partial transaction. See [`transport-i2c.md` § Bus management](transport-i2c.md#bus-management).

## `response.body()` returns null

Cause: you're running in [streaming mode](glossary.md) (no `JsonBackend`), and `body()` requires a tree to walk. Streaming-mode builds skip the JSON tree entirely — the body is dispatched as SAX events into `Rsp::Sink` instead. Fix: either pass a `JsonBackend` to the `Notecard` constructor (tree mode — `body()` then returns a walkable `JsonReader*`), or stay in streaming mode and parse the body via `req.into(my_struct).execute()` for typed extraction. See [`streaming-and-tree.md`](streaming-and-tree.md) for the trade-off between the two modes, and [`body-values.md`](body-values.md) for typed body parsing.

## consteval validation rejects a string the Notecard accepts

Cause: the typed API validates string-literal enums against the schema bundled into the codegen. If the Notecard firmware accepts a newer mode value that hasn't landed in the schema yet, the C++ side flags it as invalid. Fix: drop into the unguided escape — assign a raw string to the field and skip validation, or upgrade `note-cpp` to a build that includes the newer schema:

```cpp
auto req = nc.card.attn();
req.mode = "arm,connected,some_future_mode";   // unguided — no validation
req.execute();
```

See [`using-the-api.md` § Unguided requests](using-the-api.md#unguided-requests).

## Tests pass on host but fail on device

Cause: the host test harness uses a mock transport that's permissive — it doesn't enforce the Notecard's actual init handshake, baud timing, or CRC sequencing. The real device is stricter. Common culprits:

- **Wrong baud.** The Notecard auto-bauds, but only within a window after its own reset.
- **I2C address mismatch.** Default is `0x17`; some boards or carriers re-strap.
- **Host re-sending before the Notecard has answered.** The mock answers instantly; the device doesn't.
- **Missing line termination.** A custom serial HAL that doesn't append `\n` works against tolerant mocks but hangs on real hardware.

Fix: enable wire tracing (`NOTE_DEBUG_ENABLED=1` plus a listener that dumps both directions) and diff the byte stream against a known-working note-c run on the same board. See [`debugging.md` § Custom Listeners](debugging.md#custom-listeners).

## See also

- [`known-issues.md`](known-issues.md) — confirmed library bugs with workarounds (Apple Clang `consteval`, JSONB raw bodies)
- [`debugging.md`](debugging.md) — wire tracing, transport diagnostics, debug categories
- [`getting-started.md`](getting-started.md) — top-down walkthrough from a clean project to your first request
