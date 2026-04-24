# `tests/compile_warn/`

Compile-warn fixtures — each `.cpp` compiles clean under the default
`tests/compile_fail/` flags but fails under `-Werror=deprecated-declarations`.
`tests/CMakeLists.txt` registers them with `WILL_FAIL TRUE`, so each test
passes iff a `[[deprecated]]` attribute fires.

The intent is to lock in that a call that's **valid but discouraged**
emits a deprecation — without escalating to a hard compile error for
users who choose to accept the risk. Distinct from `compile_fail/`, which
covers **invalid** calls that must not compile under any settings.

## What's covered

### Target filtering — unsupported endpoint on a non-strict target

A non-strict target (`Target<R, ...>` or `MinFirmware<...>` with
`Strict=false`) calling an endpoint that isn't in its radios/firmware
envelope gets a `[[deprecated("... is not available on this target")]]`
overload. Under `-Werror=deprecated-declarations` this becomes an error.

- `target_lora_sleep_warns.cpp` — non-strict LoRa target calls
  `card.sleep` (WiFi-only)
- `fw_old_illumination_warns.cpp` — non-strict 5.0.0 target calls
  `card.illumination` (needs 9.1.1)
- `target_old_wifi_illumination_warns.cpp` — combined radios+firmware:
  WiFi-supported endpoint but old firmware triggers the firmware-leg
  deprecation

## When to add new fixtures

Any time the codegen introduces a new `[[deprecated]]` path, add a
fixture here that triggers it. The corresponding clean-compile case
belongs in `tests/compile_check/` (which also runs with
`-Werror=deprecated-declarations`), and the strict-reject case belongs
in `tests/compile_fail/`.
