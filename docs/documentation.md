# Documentation Generation

The `note-cpp` API reference is a Doxygen site with interactive firmware version and SKU filtering. It is generated from doc comments in the C++ headers, which are themselves generated from the Notecard API specification.

## Generating the site

```bash
./ci.sh --docs
```

This runs Doxygen and validates all internal links. The output is written to `docs/html/`. Open `docs/html/index.html` to browse locally.

Requires Doxygen 1.9.8+: `brew install doxygen` (macOS) or `apt-get install doxygen` (Ubuntu).

## Pipeline overview

```
notecard-api.openapi.json          API specification (source of truth)
        │
        ▼
tools/codegen/generate.py          Code generator (Jinja2 templates)
        │
        ├──► include/note/api/*.hpp    74 per-endpoint headers with doc comments
        ├──► include/note/api.hpp      Umbrella header / Api factory
        └──► tests/test_*.cpp          Generated test files
                │
                ▼
            Doxyfile                   Doxygen configuration
                │
                ├── include/note/          All headers (hand-written + generated)
                ├── docs/mainpage.md       Landing page content
                └── docs/groups.dox        Topic grouping (User API / Internals)
                        │
                        ▼
                docs/html/             Generated HTML site
```

## File reference

### Configuration

| File | Purpose |
|------|---------|
| `Doxyfile` | Doxygen configuration: inputs, theme, aliases, preprocessing |
| `docs/groups.dox` | Assigns types to "User API" and "Internals" topic groups |
| `docs/mainpage.md` | Landing page content (`\mainpage`) |

### Theme and filtering

| File | Purpose |
|------|---------|
| `docs/doxygen-awesome-css/` | [doxygen-awesome](https://github.com/jothepro/doxygen-awesome-css) theme (submodule) |
| `docs/doxygen-header.html` | Custom HTML header template (loads filter JS/CSS) |
| `docs/doxygen-filter.js` | Client-side firmware version and SKU filtering logic |
| `docs/doxygen-filter.css` | Filter bar styling and `.filtered-out` hide rule |

### Code generation templates

| File | Purpose |
|------|---------|
| `tools/codegen/templates/endpoint.hpp.j2` | Per-endpoint header template (doc comments, `@since{}`, `@skus{}`) |
| `tools/codegen/templates/api.hpp.j2` | `Api` factory template |
| `tools/codegen/generate.py` | Generator entry point |
| `tools/codegen/spec_parser.py` | OpenAPI spec parsing |
| `tools/codegen/model.py` | Data model (operations, properties, SKUs, versions) |

## Doc comment tags

Two custom Doxygen aliases emit HTML `data-` attributes that the filter JavaScript uses:

### `@since{X.Y.Z}` — firmware version gating

Applied to individual fields. Renders a "Since: firmware X.Y.Z" badge and emits `data-min-api-version="X.Y.Z"` for filtering.

In the Jinja2 template (`endpoint.hpp.j2`), this is generated from `PropertyDef.min_api_version`:

```
/// Description of the field.
///
/// @since{3.4.1}
```

### `@skus{CELL,WIFI,...}` — SKU availability

Applied to operation structs. Renders a "Supported SKUs" badge and emits `data-skus="CELL,WIFI,..."` for filtering.

Generated from `OperationDef.skus`:

```
/// Description of the operation.
///
/// @skus{CELL,CELL+WIFI,WIFI}
```

## Filtering behavior

The filter bar appears on pages that contain `@since{}` or `@skus{}` annotations:

- **Firmware version dropdown**: Selecting a version hides fields introduced after that version. "All versions" shows everything.
- **SKU checkboxes**: Unchecking a SKU hides operations that are only available on that SKU. All SKUs are checked by default.
- Selections persist across pages via `localStorage`.
- A member is hidden only when *all* of its annotations are filtered out.

## Topic groups

Types are organized into two groups without modifying source headers. The grouping is defined in `docs/groups.dox` using Doxygen's `@defgroup` and `@class` commands:

- **User API** — types you explicitly name in code: `Notecard`, `Api`, `ApiResult`, `JsonBuilder`, `JsonReader`, `JsonBuf`, `ErrorInfo`, duration types, `VoltageVariable`, etc.
- **Internals** — types accessed implicitly through `auto` variables and operator overloads: `Field`, `FlagSet`, `DynField`, `Target`, `StringPool`, `MonotonicArena`, `Allocator`, `SaxParser`, etc.

The `note::detail::*` namespace is fully excluded via `EXCLUDE_SYMBOLS`.

## Link validation

`./ci.sh --docs` validates all internal `href` references in the generated HTML. Every link to another page or anchor is checked to ensure the target file exists. Broken links cause the build to fail.

## Regenerating headers

If you modify `endpoint.hpp.j2` or other codegen templates, regenerate the headers:

```bash
python3 tools/codegen/generate.py notecard-api.openapi.json \
    -o include/note/api \
    --api include/note/api.hpp \
    --test-dir tests
```

Or run the full CI which includes codegen as its first step:

```bash
./ci.sh
```

In CI, the build fails if generated files are out of date (compares `git diff` after regeneration).
