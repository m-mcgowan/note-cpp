# Getting started

`note-cpp` is a typed C++ wrapper around the [Blues Notecard](https://blues.com/products/notecard/) JSON-over-wire API. The same library compiles for Arduino, ESP-IDF, Zephyr, bare-metal, and stdcpp hosts — pick a transport, build a request, get a typed response.

This page walks from a clean project to your first request. Most of it targets readers who aren't on Arduino, since the README's quickstart already covers that case in detail. If you're on Arduino, the [§ Arduino](#arduino) section below is a five-line summary that links straight into the canonical setup.

## Pick your platform

There's a 5-to-10-line install + first-build snippet for each supported platform below. Once you're past setup, every platform uses the same typed API — you can read [using-the-api.md](using-the-api.md) without re-reading this page.

### Arduino

```cpp
#include <note.hpp>

Notecard nc;

void setup() {
    nc.begin(Serial1, 9600);          // or nc.begin(Wire) for I2C
    nc.hub.set().product("com.example.app").mode("periodic").execute();
}
```

Install via **Sketch → Include Library → Add .ZIP Library** pointing at this repo's ZIP, or add `https://github.com/m-mcgowan/note-cpp.git` as a PlatformIO `lib_deps` entry. See the [Arduino guide](platforms/arduino/guide.md) for the full setup, printing patterns, and ATTN/interrupt usage.

### PlatformIO

```ini
; platformio.ini
[env:myboard]
lib_deps = https://github.com/m-mcgowan/note-cpp.git
build_flags = -std=gnu++20    ; or gnu++17 minimum
```

Use the Arduino sketch above for the Arduino framework, or the [stdcpp setup](#stdcpp--cmake-host) below if you're building against ESP-IDF or a bare-metal framework. Build with `pio run -e myboard`.

### stdcpp / CMake host

On a Linux/macOS host (or any non-Arduino target), you wire the backend and transport explicitly. Add the library:

```cmake
add_subdirectory(note-cpp)
target_link_libraries(my_app PRIVATE note-cpp)
```

A minimal `main.cpp` — replace `MySerialHal` with whatever talks to your hardware (or a mock for host-side experiments):

```cpp
#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/link/serial.hpp>
#include <note/protocol.hpp>

int main() {
    MySerialHal hal;                                 // your serial HAL impl
    note::link::SerialFramer serial_hal(hal);        // note::Hal — wire framing
    note::Protocol transport(serial_hal);            // ITransact — wire protocol

    note::Notecard nc(transport);                    // streaming — no JSON backend needed
    note::Api api(nc);

    auto r = api.card.version().execute();
    if (r) std::printf("Notecard %.*s\n", (int)r.version.size(), r.version.data());
}
```

Streaming is the recommended default — typed response fields, struct body parsing via `.into(struct)`, and the rest of the API surface work without a JSON tree-mode backend linked, and the wire response can be arbitrarily large. Switch to tree mode (`Notecard nc(backend, transport);` with a `note::backends::CjsonBackend backend;`) only when you need `response.body()` for ad-hoc field walks or the `nc.request("endpoint", [&](auto& b){ … })` lambda builder — see [`docs/streaming-and-tree.md`](streaming-and-tree.md) for the full tradeoff.

Build: `cmake -S . -B build && cmake --build build`. The runnable companion to this section is [`examples/stdcpp/getting-started.cpp`](../examples/stdcpp/getting-started.cpp), which uses a mock transport so you can experiment without hardware. See [`examples/stdcpp/README.md`](../examples/stdcpp/README.md) for the full example index.

### ESP-IDF

ESP-IDF uses the same setup as the [stdcpp / CMake host](#stdcpp--cmake-host) section above — add `note-cpp` as a CMake subdirectory and link it. ESP-IDF already bundles cJSON, so `CjsonBackend` reuses it. For the C++ standard, set `target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)` in your component's `CMakeLists.txt` (C++17 minimum, C++20 unlocks designated initializers and `consteval` validation — see [cpp-version-compatibility.md § Setting the standard in your build](cpp-version-compatibility.md#setting-the-standard-in-your-build)).

The HAL implementation for ESP-IDF UART/I2C drivers is yours to wire (the same `note::Hal` interface used by the Arduino bindings).

## Your first request

> Throughout the rest of this page, `nc` is the API surface. On Arduino, that's the `Notecard nc;` you declared. On stdcpp/ESP-IDF, after `Notecard nc(transport); Api api(nc);` you write `api.card.version()` instead — the calls are identical, the receiver isn't. See [using-the-api.md](using-the-api.md) for the full picture.

Walking through one full request — `card.version`, the simplest readable Notecard endpoint — top to bottom:

```cpp
auto r = nc.card.version().execute();
if (r) {
    // r.version  is the firmware version string (e.g. "notecard-7.2.1")
    // r.device   is the device DID
    // r.sku      is the hardware SKU
}
```

That call sends this JSON over the wire:

```json
{"req":"card.version"}
```

The Notecard answers in kind:

```json
{"version":"notecard-7.2.1","device":"dev:864475044211711","sku":"NOTE-WBNA","board":"3.2.1","api":4}
```

`note-cpp` parses that response into the typed `r` struct — each field is a `string_view` or numeric type, and presence is tracked separately from value (a Notecard that sends `"sku":""` is distinguishable from one that omits the field). `if (r)` checks the request succeeded; field-level presence is `r.sku.has_value()`. The [working-with-responses.md](working-with-responses.md) page is the full reference for response handling.

Building any other request follows the same shape: pick the endpoint (`nc.hub.set()`, `nc.note.add()`, etc.), chain typed setters, call `execute()`. There are five calling styles for the typed API and three "escape hatch" levels below it — see [using-the-api.md](using-the-api.md) for the full tour.

## Sending data

To send sensor readings to Notehub, define a struct for your data once and pass an instance as the body:

```cpp
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)   // optional on C++20
};

Readings r{.temperature = 22.5f, .humidity = 60};
nc.note.add().file("sensors.qo").body(r).execute();
```

That serializes to `{"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60}}` on the wire. The struct can be reused for receive (`.into(r)`) and for [Notecard template registration](body-values.md) (compact on-device storage). The [`examples/stdcpp/sending-notes/`](../examples/stdcpp/sending-notes/README.md) example walks every body shape — raw string, lambda builder, typed struct, template-backed, and receive — end to end.

## Sizing your build

The library scales from ATmega328P (32 KB flash / 2 KB RAM) to desktop hosts without changing the typed API. Knobs to dial:

**What's my flash budget?**

- **≥ 64 KB** — defaults are fine. The full typed API plus a JSON backend fits comfortably.
- **32 KB (Arduino Uno class)** — define `NOTE_MINIMAL`. This bundles JSONB-on, streaming mode, no retry, no request IDs, no extras — about 4-5 KB savings vs the full build. See the [Arduino guide § Binary size comparison](platforms/arduino/guide.md#binary-size-comparison) for the measured matrix across four API styles, and [`platforms/arduino/avr-guide.md`](platforms/arduino/avr-guide.md) for the AVR-specific patterns.
- **< 32 KB or Cortex-M0** — `NOTE_MINIMAL` plus the `JsonView` scan pattern (raw JSON in, hand-parsed substring lookup out). Trades type safety for ~14 KB flash; covered in [using-the-api.md § Raw JSON](using-the-api.md#raw-json).

**What's my RAM budget?**

- **Heap available, don't care about allocs** — defaults. `CjsonBackend` allocates per-node from the heap.
- **Heap allowed but want it bounded** — pair `StaticJsonBackend<N,T>` (fixed in-memory build/parse buffers, zero heap) with no arena. Response strings stay valid until the next `execute()`.
- **No heap, want response strings to outlive the next call** — streaming mode (no JSON backend) plus a `MonotonicArena`. The arena interns response strings; you reset it when you're done with a batch. See [memory.md](memory.md) for sizing.
- **No heap at all** — streaming mode plus arena, as above. Compile-time-checked: `note::Notecard nc(transport, note::arena_allocator(arena))` (where `transport` is a `note::Protocol` over your `SerialFramer`) constructs the streaming-only Notecard, which won't link if you later try to call a tree-mode-only path.

**Which API style?**

- **Typed API (default)** — `nc.hub.set().product(...).execute()`. Best DX; fits ≥ 32 KB flash.
- **Lambda request builder** — `nc.request("hub.set", [](auto& b) { ... })`. Familiar entry point if you're migrating from `note-c`. Same transport; coexists with typed in the same firmware.
- **Raw JSON** — `nc.transact(json_string, buf)` plus `note::JsonView` for response parsing. Smallest flash; you trade type safety for ~14 KB.

[`docs/feature-flags.md`](feature-flags.md) is the canonical flag reference (every flag, every default, savings per flag). [`docs/platforms/arduino/avr-guide.md`](platforms/arduino/avr-guide.md) has measured numbers for the four AVR API styles.

## What to read next

- **More on the typed API** — [using-the-api.md](using-the-api.md) (calling styles, focused operations, escape hatches)
- **Reading responses** — [working-with-responses.md](working-with-responses.md) (presence checks, body parsing, error categories)
- **Memory questions** — [memory.md](memory.md) (arenas, string lifetimes, backend memory profiles)
- **AVR / size-constrained targets** — [platforms/arduino/avr-guide.md](platforms/arduino/avr-guide.md)
- **Migrating from note-arduino** — [platforms/arduino/migration-from-note-arduino.md](platforms/arduino/migration-from-note-arduino.md)
- **Full documentation index** — [docs/README.md](README.md)
