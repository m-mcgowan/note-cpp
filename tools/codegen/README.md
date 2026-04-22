# `tools/codegen/`

Reads the Notecard OpenAPI 3.1 spec, produces the generated headers and
tests under `include/note/api/`, `include/note/api.hpp`, and
`tests/test_*.cpp` (coverage, streaming, samples, compile-checks, sizeof
report), plus `cmake/note-cpp-generated.cmake` and `docs/api-reference.md`.

For the big-picture pipeline (schema → OpenAPI → C++) see
[`docs/internal/codegen.md`](../../docs/internal/codegen.md).

## Contents

- `generate.py` — entrypoint. Called by `ci.sh` and
  `validate-release.sh`. Reads the spec, builds the intermediate
  `model.py` structures via `spec_parser.py`, applies naming via
  `naming.py`, and renders every file in `templates/`.
- `spec_parser.py` — turns the OpenAPI spec + metadata extensions into
  `Endpoint`, `PropertyDef`, `Intent`, … records defined in
  `model.py`. Knows about `x-dispatch`, `x-intents`, `x-toggle`,
  `x-action`, `x-format`, and our type refinements.
- `model.py` — plain dataclasses the templates iterate over. Keep them
  data-only; all conversion logic lives in `spec_parser.py`.
- `naming.py` — the one place that decides struct names, method
  names, factory methods, and friendly identifiers. Handles keyword
  clashes (`override` → `override_`), dispatch verb renames, etc.
- `gen_validation.py` — standalone consteval validator generator used
  by the field-type enum safety layer (`note::validated_mode`, etc.).
- [`templates/`](templates/) — every Jinja2 template that `generate.py`
  renders.
- [`metadata/`](metadata/) — the per-endpoint and per-property overlay
  JSON that rides on top of the upstream Blues schema.

## Usage

```bash
python3 tools/codegen/generate.py notecard-api.openapi.json \
    -o include/note/api \
    --api include/note/api.hpp \
    --test-dir tests
```

`ci.sh` calls this with `$ROOT/...` absolute paths; the generator
converts those back to repo-relative form when it writes
`note-cpp-generated.cmake`, so the checked-in file stays
machine-independent.

Verify it's up to date:

```bash
./ci.sh   # diffs generated files against the tree; fails CI on drift
```
