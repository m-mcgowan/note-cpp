# Release Checklist — v0.1.0-beta

## Pre-release

### Documentation review
- [ ] README reviewed — prominent "BETA" banner, API may change before v1.0
- [ ] Migration guide reviewed (all examples functionally equivalent)
- [ ] Arduino guide reviewed (printing, String conversion, debug, AVR)
- [ ] card-attn-guide.md reviewed (arm/rearm/off/on/query/sleep)
- [ ] working-with-responses.md reviewed (arrays, body parsing, lifetimes)
- [ ] body-values.md reviewed (all 6 approaches documented)
- [ ] error-handling.md reviewed (nc.execute pattern, not req.execute(nc))
- [ ] known-issues.md reviewed (Clang consteval bug documented)
- [ ] Doxygen mainpage reviewed
- [ ] CONTRIBUTING.md reviewed

### Testing on real hardware
- [ ] Integration tests pass on ESP32-S3 via serial (MPCB 1.9 or 1.10)
- [ ] Integration tests pass on ESP32-S3 via I2C
- [ ] card.attn arm/rearm/disarm/off/on round-trip verified
- [ ] Raw passthrough (transact/send) verified on device
- [ ] Debug wire output (setDebugOutput) verified on device
- [ ] Arduino CLI serial_basic sketch runs and succeeds
- [ ] Arduino CLI i2c_basic sketch runs and succeeds
- [ ] PlatformIO arduino-migration example builds and runs
- [ ] AVR binary-size-comparison builds (pio run -e avr-notecpp)

### note-c comparison
- [ ] Binary size comparison documented:
      AVR: note-cpp 14.6KB (45%) vs note-c 24.6KB (76%) — 41% smaller
      ESP32: note-cpp ~1% smaller code, ~6% smaller constants
- [ ] Document advantages: type safety, compile-time validation, zero heap,
      consteval enum/flag/JSON validation, polymorphic intents, debug hooks,
      OwnedBuffer passthrough, Printable responses
- [ ] Document limitations: C++17 minimum, no ESP8266,
      Clang consteval limitation, passthrough needs buffer on streaming path
- [ ] Heap usage measured on device (not just inferred from static analysis)

### CI
- [ ] `./ci.sh` (quick) passes
- [ ] `./ci.sh --full` passes (headers, examples, version gating, coverage)
- [ ] CMake build passes (both host and Arduino targets)
- [ ] All compile-fail and compile-check tests pass
- [ ] Coverage ≥ 90% lines, 90% functions, 85% branches
- [ ] Arduino CLI compiles both sketches

### Streaming transport
- [x] Phase 1: write()/read() on ITransport — binary streaming works
- [x] Phase 2: caller-provided buffers (set_receive_buffer, transact_into)
- [x] Phase 3: streaming SAX parser (StreamingSaxParser, SaxStreamBuf)
- [x] Binary send/receive verified on hardware (serial, 1.9 MPCB)
- [x] Streaming SAX verified on hardware (6 tests, all pass)
- [x] Serial over-read fix — binary after JSON preserved via overflow buffer
- [x] Serial segment pacing matches note-c (250 byte chunks, 250ms delay)
- [x] Design doc (streaming-transport.md) accurate

### API stability
- [ ] Public API headers reviewed for consistency
- [ ] No breaking changes planned before v0.1 tag
- [ ] Version in library.properties and library.json matches
- [ ] Note: API may change before v1.0 — v0.1-beta is a preview release

## Beta release process

### Tagging
- [ ] Tag `v0.1.0-beta.1` on main
- [ ] GitHub Release with "Pre-release" checkbox
- [ ] Release notes summarize: what works, what's beta, what to test

### Packaging
- [ ] Verify `include/` → `src/` symlink works for Arduino Library Manager
- [ ] Publish to PlatformIO registry (beta tag)
- [ ] Update README badge with version
- [ ] Remove codecov badge token from README (private repo token `?token=...` not needed once public)

### Announcement — beta testers
- [ ] Blues community forum (discuss.blues.io): post with migration guide link,
      invite 2-3 community members to port their note-c firmware
- [ ] Blues blog (blues.com/blog): technical deep-dive post —
      AVR binary size comparison, typed API examples, debug hooks
- [ ] GitHub Discussions enabled for feedback (lower friction than Issues
      for "is this the right pattern?" questions)

### Broader visibility (after beta feedback)
- [ ] Hacker News: embedded C++ with real binary size data
- [ ] r/embedded, r/cpp: "41% smaller than C on AVR" angle
- [ ] Embedded Artistry newsletter (Phillip Johnston covers modern C++ for embedded)

### Beta feedback loop
- [ ] Collect migration friction reports (like the ~/e/notecard-tests port)
- [ ] Track DX scores from testers (target: 9+/10 for common operations)
- [ ] Address string_view printing confusion (docs, not library)
- [ ] Address any new API gaps discovered by testers
- [ ] Plan v0.1.0 (non-beta) based on feedback

## v1.0 criteria (future)
- Beta feedback incorporated
- API stable — no breaking changes since last beta
- ESP8266 support (if feasible with tl::expected)
- Debug system phases 2-5 complete (timing, memory, transport, StaticNotecard policy)
- Inter-transaction timeout (note-c's _TransactionStart equivalent)
- card.aux response `state` array (array of objects)
- At least 3 community members have successfully ported from note-c
