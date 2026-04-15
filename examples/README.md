# Examples

Learn `note-cpp` by example. Start with the Arduino quickstart or the getting-started walkthrough, then explore the feature-specific examples below.

### Arduino

| Example | What you'll learn |
|---------|-------------------|
| [arduino/readme_quickstart/](arduino/readme_quickstart/) | Minimal setup — connect, configure hub, send data, read responses |
| [arduino-migration/](arduino-migration/) | Side-by-side migration from note-arduino / note-c |

### Standard C++

| Example | What you'll learn |
|---------|-------------------|

| Example | What it covers |
|---------|----------------|
| [getting_started.cpp](getting_started.cpp) | Four ways to talk to a Notecard: ad-hoc JSON, compile-time JSON, typed API, and body schemas. Start here. |
| [sending-notes/](sending-notes/) | Every way to send and receive data — raw JSON, builder lambdas, typed structs, templates for compact storage. |
| [hub-configuration/](hub-configuration/) | Connection setup: type-safe duration units, named constants, voltage-variable sync, compile-time validation. |
| [attention_pin.cpp](attention_pin.cpp) | Using the ATTN pin to wake the host MCU — arming triggers, watchdog timers, sleep with payload, state across resets. |
| [location_tracking.cpp](location_tracking.cpp) | GPS and geofencing — periodic fixes, continuous mode, fixed locations, geofence radius. |
| [target_filtering.cpp](target_filtering.cpp) | Compile-time hardware compatibility checks — constrain the API to your Notecard variant (WiFi, Cell, LoRa, Skylo). C++20. |
| [zero_alloc.cpp](zero_alloc.cpp) | Zero-allocation patterns for memory-constrained systems — buffer backend, string pool, arena allocator. |

## Building

Most examples compile and run on any machine (no Notecard hardware needed):

```bash
c++ -std=c++20 -I include examples/getting_started.cpp && ./a.out
c++ -std=c++20 -I include examples/sending-notes/main.cpp && ./a.out
c++ -std=c++20 -I include examples/hub-configuration/main.cpp && ./a.out
```

Examples that use `-fsyntax-only` (attention_pin, location_tracking, target_filtering)
verify compilation but don't produce a runnable binary — they demonstrate API usage
patterns for code that would run on real hardware.

All example code is compiled as part of CI — every snippet you see in the
documentation is verified to be syntactically correct and up to date.

## Mock backend

Examples use a shared [mock_backend.hpp](mock_backend.hpp) that builds valid JSON
for requests and returns empty responses. On real hardware, you'd use a JSON backend
(cJSON, nlohmann-json, etc.) and a transport (serial or I2C) — see the commented
example in getting_started.cpp.
