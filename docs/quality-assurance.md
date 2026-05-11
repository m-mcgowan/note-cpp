# Quality assurance

`note-cpp` tests exercise every code path on the platforms users deploy to, with coverage tracked on both host and embedded targets. The documentation is verified at push time so examples in the docs can't drift from working code.

## Library code

The portable test suite is one set of `.cpp` files compiled into multiple binaries — host doctest binaries that run in CI under five compilers, and an ESP32-S3 firmware that flashes onto a real Notecard. The same `TEST_CASE` — same assertions, same fixtures — runs on both. Backend-specific paths (cJSON, nlohmann/json, the `StaticJsonBackend` SAX backend) are exercised on both targets so behaviour stays in lock-step.

| Level | What | Count |
|-------|------|-------|
| **Host unit tests** | doctest tests covering all endpoints, transport, SAX parsing, body structs, error handling | ~1,872 test cases |
| **Arduino host build** | Same sources compiled with `ARDUINO` + stubs, verifying `Printable` integration | ~1,889 test cases |
| **Backend parity** | cJSON / nlohmann / `StaticJsonBackend` run on host and device from one source | 89 test cases |
| **On-device firmware** | ESP32-S3 with a real Notecard over serial/I2C — runs the portable suite plus fixture tests for live API requests, binary transfer, streaming SAX | portable + device-only fixture tests |
| **Wokwi runtime (AVR)** | Examples run on a simulated ATmega328P via `wokwi-cli` (CI) and the VS Code Wokwi extension (local) — catches stack-overflow / Arduino-init failures that static build verification can't | per-example smoke tests |
| **Compile-fail tests** | Verify that invalid API usage doesn't compile (wrong types, invalid flags, bad JSON) | 19 |
| **Multi-compiler CI** | g++ 12/13/14, clang++ 17/18, C++20 and C++23, libstdc++ and libc++ | 5 configurations |
| **AVR build verification** | ATmega328P (Arduino Uno) binary size checks across four API styles | PlatformIO |
| **Embedded compatibility** | Library examples compiled across ESP32, AVR, STM32 via [compat-check](https://github.com/m-mcgowan/embedded-cpp-compat-check) | CI |

Coverage is tracked on both targets:

- **Host:** GCC + lcov 2.x — lines 97.5%, functions 99.0%, branches 96.2%. Enforced in CI.
- **Embedded (ESP32-S3):** lines 81.6%, functions 82.7%, gathered via [pio-cov](https://github.com/m-mcgowan/pio-cov), a PlatformIO-aware coverage runner.

Host tests run in ~35 seconds. The full CI matrix (5 compilers + coverage + embedded compat + Wokwi runtime) runs on every push.

## Documentation

The library docs are kept honest by automated checks that run pre-push and in CI:

- **Internal link verification** — every internal Markdown link in the docs tree is resolved at push time. Broken links fail the pre-push hook before they reach the remote.
- **Live code snippets** — examples in the docs are injected from compiled source files via `<!-- snippet: -->` markers, so the code in a doc is the same code that builds in CI. They can't drift from working examples.
- **Migration-table alignment** — the side-by-side note-c ↔ note-cpp tables in the migration guide are kept column-aligned by tooling, so the layout doesn't degrade as patterns are added.

All three checks run via [`tools/verify-docs.sh`](../tools/verify-docs.sh), wired into both the `pre-push` git hook ([`.githooks/pre-push`](../.githooks/pre-push)) and the main CI pipeline (`ci.sh`). Failures block.
