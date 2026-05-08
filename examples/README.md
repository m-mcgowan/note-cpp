# Examples

Learn `note-cpp` by running code. The two top-level entry points are platform-shaped: if you're on an embedded MCU and PlatformIO or the Arduino IDE is your build system, start with [`arduino/quickstart/`](arduino/quickstart/) — minimal sketch, real Notecard. If you're on a Linux/macOS host or in a CMake build, start with [`stdcpp/getting-started.cpp`](stdcpp/getting-started.cpp) — host-runnable, no hardware needed.

The feature-specific examples below assume you've worked through one of those entry points; each one zooms in on one concept (sending notes, hub configuration, IntelliSense) without the setup overhead.

### Arduino

| Example | What you'll learn |
|---------|-------------------|
| [arduino/quickstart/](arduino/quickstart/) | Minimal setup — connect, configure hub, send data, read responses |
| [arduino/serial_basic/](arduino/serial_basic/) | Serial transport setup |
| [arduino/i2c_basic/](arduino/i2c_basic/) | I2C transport setup |
| [arduino/migration/](arduino/migration/) | Side-by-side migration from note-arduino / note-c |

### Standard C++

| Example | What it covers |
|---------|----------------|
| [stdcpp/getting-started.cpp](stdcpp/getting-started.cpp) | Four ways to talk to a Notecard: ad-hoc JSON, compile-time JSON, typed API, and body schemas. Start here. |
| [stdcpp/sending-notes/](stdcpp/sending-notes/) | Every way to send and receive data — raw JSON, builder lambdas, typed structs, templates for compact storage. |
| [stdcpp/hub-configuration/](stdcpp/hub-configuration/) | Connection setup: type-safe duration units, named constants, voltage-variable sync, compile-time validation. |
| [stdcpp/attention-pin.cpp](stdcpp/attention-pin.cpp) | Using the ATTN pin to wake the host MCU — arming triggers, watchdog timers, sleep with payload, state across resets. |
| [stdcpp/location-tracking.cpp](stdcpp/location-tracking.cpp) | GPS and geofencing — periodic fixes, continuous mode, fixed locations, geofence radius. |
| [stdcpp/target-filtering.cpp](stdcpp/target-filtering.cpp) | Compile-time hardware compatibility checks — constrain the API to your Notecard variant (WiFi, Cell, LoRa, Skylo). C++20. |
| [stdcpp/zero-alloc.cpp](stdcpp/zero-alloc.cpp) | Zero-allocation patterns for memory-constrained systems — buffer backend, string pool, arena allocator. |

## Building

Most stdcpp examples compile and run on any machine (no Notecard hardware needed):

```bash
c++ -std=c++20 -I include examples/stdcpp/getting-started.cpp && ./a.out
c++ -std=c++20 -I include examples/stdcpp/sending-notes/main.cpp && ./a.out
c++ -std=c++20 -I include examples/stdcpp/hub-configuration/main.cpp && ./a.out
```

Examples that use `-fsyntax-only` (attention-pin, location-tracking, target-filtering)
verify compilation but don't produce a runnable binary — they demonstrate API usage
patterns for code that would run on real hardware.

All example code is compiled as part of CI — every snippet you see in the
documentation is verified to be syntactically correct and up to date.

## Mock backend

Examples use a shared [mock_backend.hpp](stdcpp/mock_backend.hpp) that builds valid JSON
for requests and returns empty responses. On real hardware, you'd use a JSON backend
(cJSON, nlohmann-json, etc.) and a transport (serial or I2C) — see the commented
example in getting-started.cpp.
