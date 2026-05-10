# `examples/stdcpp/`

Host-runnable examples — everything here compiles with a normal
`c++ -std=c++20 -I include …` command line, no Arduino or PlatformIO
required. Use these to explore `note-cpp` on your workstation before
flashing to hardware, or as copy-paste starting points for real
projects.

See [`../README.md`](../README.md) for the full set including Arduino.

## Contents

### Runnable examples

Each of these compiles to a standalone binary that runs on macOS /
Linux (no Notecard hardware needed — they use the shared
`mock_backend.hpp`):

- `getting-started.cpp` — the canonical tour. Four ways to talk to a
  Notecard (ad-hoc JSON, compile-time JSON, typed API, body schemas),
  end to end.
- `zero-alloc.cpp` — zero-allocation patterns for memory-constrained
  systems: `StaticJsonBackend`, `StringPool`, `MonotonicArena`.
- `note-c-bridge.cpp` — implementing `ITransact` on top of
  `note-c`'s `NoteRequestResponseJSON()`. The migration-bridge
  pattern for projects that already have `note-c` wiring they don't
  want to rip out.

### Syntax-only examples

These use `-fsyntax-only` in CI — they demonstrate API shapes for
code that would run on real hardware but don't produce an executable
here:

- `attention-pin.cpp` — ATTN pin patterns (arm, rearm, off, sleep
  with payload, wake-and-resume).
- `location-tracking.cpp` — GPS patterns (periodic, continuous,
  fixed location, geofence).
- `target-filtering.cpp` — `note::Target<Radios::WiFi, MinFirmware<...>>`
  compile-time hardware/firmware filtering (C++20).

### Multi-file sub-examples

Examples big enough to deserve their own directory:

- [`hub-configuration/`](hub-configuration/) — type-safe durations,
  named constants, voltage-variable sync, compile-time validation.
- [`sending-notes/`](sending-notes/) — the six body-setter shapes
  (raw string, `json_fmt`, lambda, typed struct, template
  registration, body-capture arena).

### Shared support

- `mock_backend.hpp` — include-only `ITransact` mock that
  every stdcpp example uses. Builds valid JSON for requests and
  returns empty (or canned) responses so the examples run without a
  real Notecard connected.

## Building

```bash
c++ -std=c++20 -I include examples/stdcpp/getting-started.cpp && ./a.out
```

`ci.sh` builds every `.cpp` in this directory on every CI run.
