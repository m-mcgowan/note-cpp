# `tools/codegen/templates/`

Jinja2 templates rendered by `generate.py` to produce the generated
C++ headers, tests, and API reference. Each template consumes the
intermediate model built in `tools/codegen/model.py`.

## Contents

- `endpoint.hpp.j2` — per-endpoint header. Renders the request builder
  struct, response parser, factory methods, field metadata,
  `x-intents` per-intent struct variants, and the `safety` /
  `arena_size` / wire-name constants. Output: `include/note/api/<endpoint>.hpp`.
- `api.hpp.j2` — umbrella `Api<Target>` façade. Renders the resource
  groups (`nc.hub`, `nc.card.attn`, `nc.note`, …) that tie every
  endpoint together. Output: `include/note/api.hpp`.
- `api_reference.md.j2` — user-facing API reference rendered from the
  same model. Output: `docs/api-reference.md`.
- `test_endpoint_coverage.cpp.j2` — one request-builder test + one
  response-parser test per operation (every setter exercised, every
  response field populated via `PopulatedJsonReader`). Output:
  `tests/test_endpoint_coverage.cpp`.
- `test_endpoint_streaming.cpp.j2` — mirror of the above but
  exercising the streaming transport path. Output:
  `tests/test_endpoint_streaming.cpp`.
- `test_samples.cpp.j2` — wire-format round-trip tests generated from
  `x-samples` examples in the spec. Output: `tests/test_samples.cpp`.
- `test_api_context.cpp.j2` — compile-time sanity checks that each
  endpoint hangs off the right resource group on `Api<>`. Output:
  `tests/test_api_context.cpp`.
- `test_sizeof_report.cpp.j2` — runtime report printing the `sizeof`
  of every generated request type. Useful for RAM-budget diffing.
  Output: `tests/test_sizeof_report.cpp`.
- `compile_check_api_groups.cpp.j2` — compile-check that every
  generated type is reachable via the `Api<>` group accessors. Output:
  `tests/compile_check/api_groups.cpp`.

## Editing

- Template changes always go together with a regeneration
  (`./ci.sh`). CI fails if the generated files drift from the
  templates.
- Filter functions used inside the templates live in
  `tools/codegen/generate.py` (search for `env.filters[...]`). Add
  new filters there, not in individual templates.
- The model passed to every template is read-only — if you need a
  new field, add it to `tools/codegen/model.py` and populate it in
  `tools/codegen/spec_parser.py`.
