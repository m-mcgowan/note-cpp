# Release Checklist — v0.1

## Pre-release

### Documentation review
- [ ] README reviewed for accuracy and completeness
- [ ] Migration guide reviewed (all examples functionally equivalent)
- [ ] body-values.md reviewed (all 6 approaches documented)
- [ ] raw-requests.md reviewed (escape hatch documented)
- [ ] known-issues.md reviewed (Clang consteval bug documented)
- [ ] Doxygen mainpage reviewed
- [ ] CONTRIBUTING.md reviewed
- [ ] json-fmt-design.md reviewed or removed (if implemented)

### Testing on real hardware
- [ ] Integration tests pass on ESP32-S3 via serial (MPCB 1.9 or 1.10)
- [ ] Integration tests pass on ESP32-S3 via I2C
- [ ] Arduino CLI serial_basic sketch runs and succeeds
- [ ] Arduino CLI i2c_basic sketch runs and succeeds
- [ ] PlatformIO arduino-migration example builds and runs

### note-c comparison
- [ ] Binary size comparison is fair (same backend, same operations)
- [ ] Document advantages: type safety, compile-time validation, zero heap,
      consteval enum/flag/JSON validation, polymorphic intents
- [ ] Document disadvantages if any: C++17 minimum, no AVR/ESP8266 support,
      larger static RAM (BufferJsonBackend), Clang consteval limitation
- [ ] Heap usage measured on device (not just inferred from static analysis)

### CI
- [ ] `./ci.sh` (quick) passes
- [ ] `./ci.sh --full` passes
- [ ] CMake build passes (both host and Arduino targets)
- [ ] All compile-fail tests pass
- [ ] Coverage ≥ 90% lines, 90% functions, 85% branches
- [ ] Arduino CLI compiles both sketches

### Streaming transport
- [x] Phase 1: write()/read() on ITransport — binary streaming works
- [x] Phase 2: caller-provided buffers (set_receive_buffer, transact_into)
- [x] Phase 3: streaming SAX parser (StreamingSaxParser, SaxStreamBuf)
- [x] Binary send/receive verified on hardware (serial, 1.9 MPCB)
- [x] Streaming SAX verified on hardware (6 tests, all pass)
- [x] Serial over-read fix — binary after JSON preserved via overflow buffer
- [x] Design doc (streaming-transport.md) accurate

### API stability
- [ ] Public API headers reviewed for consistency
- [ ] No breaking changes planned before v0.1 tag
- [ ] Version in library.properties and library.json matches
- [ ] Note: API may change before v1.0 — v0.1 is a preview release

## Release process
- [ ] Tag `v0.1.0` on main
- [ ] Copy `include/` to `src/` for Arduino Library Manager (or verify
      symlink works on target platforms)
- [ ] Publish to PlatformIO registry
- [ ] Update README badge with version
