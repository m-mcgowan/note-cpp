# `tools/codegen/metadata/`

Extension metadata that layers on top of the upstream Blues
[`notecard-schema`](https://github.com/blues/notecard-schema) to drive
`note-cpp`-specific code generation. Everything in here is a codegen
input — the files are read by `schema_to_openapi.py` (which builds
`notecard-api.openapi.json`) and `spec_parser.py` (which parses the
spec into `model.py` records).

See [`docs/internal/codegen.md`](../../../docs/internal/codegen.md) for
the full pipeline overview. That doc walks through how the overlays
below combine with the upstream schema files to produce the generated
C++.

## Contents

- `safety_semantics.json` — maps each Notecard endpoint to HTTP
  method(s) (GET/PUT/POST/DELETE), encoding the idempotency / safety
  class and, for polymorphic endpoints, the field presence/absence
  constraints that pick a verb. Also drives `x-dispatch` generation.
- `property_extensions.json` — per-property overlay. Keys are
  `endpoint.method.property`; values inject extensions like
  `x-format: "voltage-variable"`, `x-toggle`, `x-action`,
  `x-combo-flag`, etc. into the property schema before codegen sees
  it.
- `operation_extensions.json` — per-operation overlay (peer of
  `property_extensions.json` but scoped to the operation rather than
  a single property). Carries `x-intents` definitions (arm / sleep /
  retrieve / …) and `x-intent-name` renames (e.g. dispatch verb
  `get` → intent method `read`).
- `binary_transfer.json` — flags the endpoints that follow their JSON
  handshake with raw COBS binary data (currently `card.binary put`
  and `card.binary get`). Used to generate the streaming-body
  builders.
- `binary_buffer.json` — per-endpoint maximum data payload sizes for
  the binary-transfer endpoints; gets baked into the generated
  `MAX_PAYLOAD` constants.
- `type_refinements.json` — per-field-name C++ storage type overrides
  (`"movements": "uint16_t"`, etc.). Applied last, after OpenAPI type
  mapping.

## Editing

Direct edits to these files are the supported way to add / modify
generated C++ behavior. **Do not edit the generated files under
`include/note/api/` or `notecard-api.openapi.json` — both are
rebuilt on every `./ci.sh` run.**

After editing:

```bash
# Regenerate notecard-api.openapi.json from upstream schema + overlays
python3 tools/schema_to_openapi.py /path/to/notecard-schema \
    -o notecard-api.openapi.json

# Or, if only the overlay changed (faster):
python3 tools/schema_to_openapi.py update-extensions notecard-api.openapi.json

# Then regenerate C++
python3 tools/codegen/generate.py notecard-api.openapi.json \
    -o include/note/api --api include/note/api.hpp --test-dir tests
```

`./ci.sh` does both passes in order.
