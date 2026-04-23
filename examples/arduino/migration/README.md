# note-arduino → note-cpp migration example

Runnable Arduino sketch mirroring every before/after snippet in
[`docs/platforms/arduino/migration-from-note-arduino.md`](../../../docs/platforms/arduino/migration-from-note-arduino.md).
The doc embeds these functions by line number, so this file is the
source of truth — edit this first, then run `tools/verify-docs.sh` to
check for drift.

## What it covers

Each top-level function in `src/main.cpp` is one migration pattern:

| Function | Pattern |
|----------|---------|
| `hub_set_fluent`, `hub_set_direct` | `hub.set` — fluent chain vs. direct assignment with conditionals |
| `note_add`, `note_add_errors` | `note.add` — sending typed bodies, with error handling |
| `note_template` | `note.template` — registering typed storage layouts |
| `card_temp`, `card_temp_configure` | `card.temp` — reading device temperature |
| `card_version` | `card.version` — reading device info |
| `keep_response` | Response lifetime — when to copy fields vs. keep the response |
| `card_attn_arm`, `card_attn_disarm`, `card_attn_sleep`, `card_attn_retrieve` | `card.attn` — arming interrupts, sleep with payload |
| `env_get`, `env_set_default` | `env.get` / `env.default` — environment variables |
| `error_handling`, `error_details` | `Result<T>` error handling |
| `hub_sync` | Fire-and-forget commands (`.command()` vs. `.execute()`) |

## Build

```bash
pio run -d examples/arduino/migration -e esp32s3
```

The goal is compile-only verification — it's not meant to run against
real hardware. `platformio.ini` targets an ESP32-S3 devkit but the
code is platform-agnostic (uses `Serial1` and the generic Arduino HAL).

## When to update

If you edit `src/main.cpp`:

1. Line numbers in `docs/platforms/arduino/migration-from-note-arduino.md`
   reference this file via embedme-style snippets.
2. Run `tools/verify-docs.sh` to catch mismatches.
3. If a snippet is also in the root `README.md`, update that too.

## See also

- [migration guide](../../../docs/platforms/arduino/migration-from-note-arduino.md) — side-by-side note-c ↔ note-cpp
- [`examples/stdcpp/note-c-bridge.cpp`](../../stdcpp/note-c-bridge.cpp) — run note-cpp alongside note-c during incremental migration
