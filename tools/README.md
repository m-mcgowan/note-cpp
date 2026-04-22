# `tools/`

Author-side tooling for `note-cpp` — code generation, docs verification,
coverage triage, and build-size reports. None of it ships to library
consumers; everything here is for maintainers and CI.

## Contents

### Code generation

- [`codegen/`](codegen/) — the full OpenAPI-to-C++ generator (reads
  `notecard-api.openapi.json`, emits `include/note/api/*.hpp`,
  `docs/api-reference.md`, and several generated test files). Start
  here if you're touching the request/response API surface.
- `schema_to_openapi.py` — converts the upstream Blues
  [`notecard-schema`](https://github.com/blues/notecard-schema) JSON
  schemas to the OpenAPI 3.1 spec the generator consumes, layering in
  the metadata under `codegen/metadata/` (safety, dispatch intents,
  extensions). Run this when upstream schemas change.
- `openapi_to_schema.py` — the reverse direction; occasionally useful
  when reconciling our overlay with upstream.
- `verify_roundtrip.py` — sanity-checks that
  `schema → openapi → schema` is lossless for the properties we rely
  on.

### Documentation

- `inject-snippets.py` — resolves `<!-- snippet:… -->` markers in
  markdown files by pulling code from real compiled sources. Run with
  `--check` in CI, `--inject` when editing. See
  [`docs/internal/codegen.md`](../docs/internal/codegen.md) for the
  snippet marker syntax.
- `verify-docs.sh` — orchestrates snippet verification plus doc-link
  and api-reference freshness checks across every tracked `.md`.
  Called from `ci.sh --full` and the pre-push git hook.
- `pad_migration_tables.py` — pads the side-by-side note-c/note-cpp
  tables in `docs/platforms/arduino/migration-from-note-arduino.md`
  so the GitHub-rendered columns line up.

### Size + coverage reports

- [`binary-size-comparison/`](binary-size-comparison/) — PlatformIO
  project that builds the same 8-endpoint app five different ways on
  an Arduino Uno and emits a flash/RAM comparison. Drives the "How It
  Scales" table in the main README.
- `arena_sizing_report.py` — tabulates `max_arena_size` across every
  generated endpoint; used when tuning `MonotonicArena` defaults.
- `coverage-gaps.py` — parses an LCOV tracefile and surfaces the
  biggest uncovered files/functions. Feed it `coverage/coverage.lcov`
  after `./ci.sh --coverage`.
- `coverage-diff.sh` — before/after diff between two LCOV tracefiles.
  Useful while iterating on a test-coverage gap.
- `size_report.sh` — stdcpp host build with `-Os`; prints the linker's
  section sizes for quick "did this header bloat things?" checks.

### Ad-hoc / one-off

- `restructure-examples.sh` — untracked migration helper from the
  2026-04 platform-first examples reorg. Safe to delete once the next
  session confirms no stragglers.

## Metadata

Previously `tools/` held six loose JSON metadata files alongside the
scripts above. Those have moved to
[`codegen/metadata/`](codegen/metadata/) — they're codegen inputs and
nothing else consumes them.
