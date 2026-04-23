# Strict Body-Field Validation

## Background

`note-cpp` operates on user-defined schema structs through four independent
paths:

| Path                  | Entry point                        | Where            |
|-----------------------|------------------------------------|------------------|
| Streaming deser       | `StructSink<T>`                    | `struct_sink.hpp`|
| Random-access deser   | `parse<T>(JsonReader&)`            | `body.hpp`       |
| Serialisation         | `make_schema_body<T>`              | `body.hpp`       |
| Template registration | `template_of<T>()`                 | `body.hpp`       |

Before this feature, each path used an `if constexpr` chain with **no
terminal `else`**. Any field whose type didn't match a branch was silently
skipped. Three consequences:

- Users couldn't tell that the library didn't know how to handle a field —
  requests just had the field missing, responses left it zero-initialised.
- The ser and deser type sets were **asymmetric**: deser understood
  `std::array` and `char[N]`, ser didn't. A struct that round-tripped
  through one path quietly dropped data through another.
- The `char[N]` template hint emitted `"1"` (TSTRING(1)) instead of an
  N-char string, so the Notecard registered the wrong maximum length and
  truncated runtime values.

## Goal

Turn silent drops into compile-time errors by default, gated by
`NOTE_STRICT_BODY_FIELDS` (default `1`). Define a single set of supported
field types and make sure all four paths honour it identically.

## The split

Two different mechanisms are needed because the paths have different
call patterns:

### Streaming deser: per-handler `handles_v`, dispatch filtering

`StructSink<T>` routes SAX events through `SaxAssign*` handlers. Each
handler is invoked for **every** field of the struct that the JSON key
matches, because the dispatch is template-expanded over all fields. A
single `static_assert` in the handler body would fire for any field the
handler doesn't accept — even when another handler (like `SaxAssignInt`)
does accept it.

Fix: each handler exposes `handles_v<T>` declaring what it claims.
`sax_dispatch` filters on this at compile time:

```cpp
template<typename Handler, std::size_t N, typename R, typename T>
inline bool sax_try_field(T& obj, string_view key, Handler& handler) {
    using F = std::remove_cv_t<reflect::member_type<N, R>>;
    if constexpr (handler_accepts_v<std::remove_cvref_t<Handler>, F>) {
        if (key == reflect::member_name<N, R>()) {
            handler(reflect::get<N>(obj));
            return true;
        }
    }
    return false;
}
```

Handlers only instantiate for fields their `handles_v` claims. The silent
no-op case is eliminated structurally — by the time a handler's body runs,
it's guaranteed to have a compatible field.

`handler_accepts_v` (in `body.hpp`) defaults to `true` for handler types
without a `handles_v` trait, so generic lambda callers of
`_note_fields_dispatch` (e.g. `json_scan.hpp`) still work.

The `NOTE_FIELDS` macro dispatch applies the same filter:

```cpp
#define _NOTE_FIELDS_DISPATCH_FIELD(obj, k, handler, field)           \
    if constexpr (::note::detail::handler_accepts_v<...handler...,    \
                  ...field-type...>) {                                \
        if ((k) == #field) { (handler)((obj).field); return true; }  \
    }
```

### Ser / random-access deser / template: terminal-else `static_assert`

These three helpers are called **once per field** (not once per wire
event). Each helper's `if constexpr` chain enumerates every supported
type, and the terminal `else` is the assertion site:

```cpp
template<typename V>
void write_field_value(JsonBuilder& b, string_view name, const V& value) {
    if constexpr (std::is_same_v<V, bool>) { ... }
    else if constexpr (...) { ... }
    else if constexpr (is_schema_struct_v<V>) { ... }
    else if constexpr (is_std_array_v<V>) { ... }
    else {
#if NOTE_STRICT_BODY_FIELDS
        static_assert(sizeof(V) == 0, "note-cpp: ...");
#else
#pragma message("note-cpp: NOTE_STRICT_BODY_FIELDS=0 — ...")
#endif
    }
}
```

The chain itself is the specification — no separate predicate table to
keep in sync.

## Shared helpers

To avoid drift between the C++17 NOTE_FIELDS macro helpers and the C++20
reflected helpers (which had nearly-identical `if constexpr` chains
before), both now route through a single shared function:

| Helper pair                              | Shared implementation |
|------------------------------------------|-----------------------|
| `_note_write_field`, `write_field`       | `write_field_value`   |
| `_note_read_field`, `read_field`         | `read_field_value`    |
| `write_template_hint(b, name, V&)`       | `write_template_hint_for<V>` |

`SaxAssignString` pulls its string-assignment sub-branches out into a
shared helper when the random-access deser path (`read_field_value`)
needs the same logic.

## Type coverage

A struct field is supported on any path if its type is one of:

- `bool`
- Integral (excluding `bool`)
- Floating-point
- `char[N]`
- `note::string_view`
- Any type convertible to `string_view` (e.g. `std::string`, `const char*`)
- Any type constructible from `string_view` (e.g. some explicit-ctor string
  types)
- Any type constructible from `(const char*, size_t)` (Arduino `String`
  with that ctor)
- Any type constructible from `const char*` (relies on pool null-termination
  guarantee; empty-string case default-constructs)
- Any type with `.c_str()` returning `const char*` (Arduino `String` — ser
  path only)
- A nested schema struct (NOTE_FIELDS-registered or C++20 reflectable
  aggregate)
- `std::array<Primitive, N>` where `Primitive` is one of the above (ser and
  template). Streaming deser also supports `std::array<SchemaStruct, N>`.

## Known asymmetries

Paths aren't fully symmetric in what `std::array` element types they
support. Strict mode surfaces these as compile errors pointing to the
path that can't handle the shape:

| Element type         | Stream deser | Rand-access deser | Ser | Template |
|----------------------|:-:|:-:|:-:|:-:|
| Primitive            | ✓ | ✗ (string only)   | ✓   | ✓        |
| Nested schema struct | ✓ | ✗                 | ✓   | gated    |
| `std::array<…>`      | ✗ | ✗                 | ✗   | ✗        |

Array-of-struct ser uses `JsonBuilder::begin_element_object()` — a
keyless-object primitive added alongside this feature. Every concrete
backend (buffer, streaming, nlohmann, cJSON, jsonb, test-only) overrides
it.

### Nested templates gated pending Notecard confirmation

`detail::notecard_supports_nested_templates_v` is a `constexpr bool`
in `body.hpp`, currently `false`. When `false`, `template_of<T>()`
`static_assert`s on any field whose template shape would be nested —
that is, a schema-struct field whose hint would emit `{...}`, or an
array-of-struct field whose hint would emit `[{...}]`.

Ser is independent of this flag: `make_schema_body<T>()` emits the
full nested body regardless.

The probe cases in `tests/integration/shared/test_notecard_api.cpp`
build the nested template body by hand via a lambda body (`note::body`
+ direct `JsonBuilder` calls) so they compile regardless of the flag's
setting — the whole point of the probe is to tell us what the flag
should be. Each case logs whether the Notecard accepted or rejected
the template.

Flip procedure:
1. Run the four `note.template` probe cases against a real Notecard.
2. If the nested and array-of-struct cases log "ACCEPTED", set
   `notecard_supports_nested_templates_v = true`.
3. Remove the `if constexpr (note::detail::notecard_supports_nested_
   templates_v)` guards around the `template_body<…>()` assertions in
   `tests/test_struct_field_symmetry.cpp`.

### Other gaps

Nested arrays (`std::array<std::array<…>, N>`) still aren't supported —
that would need a `begin_element_array()` primitive on top.

Random-access deser of any `std::array` (except `std::array<string_view,
N>`) is still flagged. `JsonReader` only exposes `get_string_array`
today; extending it is out of scope. Users needing array fields should
stick to streaming deser via `.into(struct)` on a request builder.

## Macro gate behaviour

```cpp
#ifndef NOTE_STRICT_BODY_FIELDS
#define NOTE_STRICT_BODY_FIELDS 1
#endif
```

- `=1` (default): terminal-else fires `static_assert(sizeof(V) == 0, ...)`
  with a message naming the supported types and pointing to the macro.
- `=0`: terminal-else fires `#pragma message(...)` as a non-fatal
  compile-time notice, and the silent-drop behaviour from before this
  feature is preserved.

The `SaxAssign*` handler bodies keep a `static_assert(handles_v<V>, ...)`
as a contract check independent of `NOTE_STRICT_BODY_FIELDS` — its
failure means the dispatch filter and the handler's claimed set got out
of sync, which is a library bug, not a user-field-type issue.

## Template bug fixes piggybacked on the rewrite

- `char[N]` now registers TSTRING(N). `write_template_hint_for` emits an
  N-character filler string (`'x'` × N) instead of the old `"1"`.
- Nested aggregates and `std::array` fields now appear in templates.
  Previously they dropped silently, causing the Notecard to register a
  template with missing fields.

## Tests

- `tests/test_struct_field_symmetry.cpp` — one TEST_CASE per supported
  field type, exercising ser / template / round-trip through streaming
  deser. A `RootStrippingSink` adapter handles the outer `{...}` that
  the JSON parser emits but `StructSink` doesn't expect.
- `tests/compile_fail/strict_body_fields_ser.cpp` and
  `strict_body_fields_template.cpp` — verify that a struct with a
  `std::vector<int>` field (no handler claims it) fails to compile
  under `NOTE_STRICT_BODY_FIELDS=1`.

## Where things live

- `src/note/body.hpp` — `is_char_array_v`, `has_c_str_v`, `is_std_array_v`,
  `handler_accepts_v`, `is_schema_struct_v`, `write_field_value`,
  `read_field_value`, `write_template_hint_for`, `char_array_template_filler`,
  `NOTE_FIELDS` macro plus the `_note_fields_write_hints` helper it emits.
- `src/note/struct_sink.hpp` — `SaxAssign*` handler bodies (with
  `handles_v<T>`), filtered `sax_dispatch`, `SaxCaptureChildCreator`,
  `SaxCaptureArray`, `SaxDetectFieldKind` (each with `handles_v`),
  `array_elem_vtable_for` (instantiates handlers only for compatible
  element types).
