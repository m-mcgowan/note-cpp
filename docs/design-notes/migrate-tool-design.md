# Migration Tool Design — tools/migrate.py

## Purpose

Automate conversion of note-c / note-arduino code to note-cpp. Handles
mechanical transforms fully, builds models for analyzable patterns, and
flags complex patterns for manual review.

## Approach

The tool analyzes C++ source files, tracking `J*` pointer flow through
functions. It builds a request/response model for each Notecard
transaction, then emits the equivalent note-cpp code.

## J* Pointer Tracking

The script must track `J*` pointers across all declaration contexts:

```cpp
// Standalone declaration
J* req = notecard.newRequest("hub.set");

// Init-statement (if / while / for — same grammar)
if (J* rsp = notecard.requestAndResponse(req)) { ... }
while (J* rsp = poll()) { ... }
for (J* item = first(); item; item = next(item)) { ... }

// Ternary
const char* s = rsp ? JGetString(rsp, "field") : nullptr;

// Return from function
J* execute_web_get(...) { return notecard.requestAndResponse(req); }

// Passed as argument
process_response(notecard.requestAndResponse(req));
```

For Tier 1: handle standalone declarations and init-statements.
Flag ternary, return, and argument contexts for manual review.

## Tier 1 — Mechanical Transforms (fully automated)

### Include and type changes
- `#include <Notecard.h>` → `#include <note/arduino.hpp>`
- `Notecard notecard;` → `note::arduino::Notecard nc;`
- `notecard.begin(...)` → `nc.begin(...)` (same API)
- `notecard.setDebugOutputStream(Serial)` → `nc.setDebugOutput(Serial)`
- `notecard.deleteResponse(rsp);` → delete the line
- `JDelete(rsp);` → delete the line (RAII)

### Simple request chains
Input:
```cpp
J* req = notecard.newRequest("hub.set");
JAddStringToObject(req, "product", productUID);
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 5);
notecard.sendRequest(req);
```

Output:
```cpp
nc.hub.set()
    .product(productUID)
    .mode("periodic")
    .outbound(5_mins)
    .execute();
```

### Simple response parsing
Input:
```cpp
J* rsp = notecard.requestAndResponse(notecard.newRequest("card.version"));
if (rsp) {
    const char* ver = JGetString(rsp, "version");
    const char* dev = JGetString(rsp, "device");
    Serial.printf("ver=%s dev=%s\n", ver, dev);
    notecard.deleteResponse(rsp);
}
```

Output:
```cpp
auto r = nc.card.version().execute();
if (r) {
    Serial.print("ver="); Serial.print(r.version);
    Serial.print(" dev="); Serial.println(r.device);
}
```

## Tier 2 — Analyzable (script builds model, generates code)

### Conditional field addition
Input:
```cpp
J* req = notecard.newRequest("hub.set");
if (productUID && *productUID) {
    JAddStringToObject(req, "product", productUID);
}
if (use_continuous_mode) {
    JAddStringToObject(req, "mode", "continuous");
    JAddBoolToObject(req, "sync", true);
} else {
    JAddStringToObject(req, "mode", "periodic");
}
JAddNumberToObject(req, "outbound", sync_period_minutes);
```

Output:
```cpp
auto req = nc.hub.set();
if (productUID && *productUID) {
    req.product(productUID);
}
if (use_continuous_mode) {
    req.mode("continuous").sync(true);
} else {
    req.mode("periodic");
}
req.outbound(sync_period_minutes);
req.execute();
```

The script detects conditional blocks that modify the same `J*` pointer
and preserves the conditional structure around the fluent setters.

### Body construction
Input:
```cpp
J* body = JCreateObject();
JAddStringToObject(body, "message", msg);
JAddNumberToObject(body, "ts", (double)millis());
JAddItemToObject(req, "body", body);
```

Output:
```cpp
req.body(R"({"message":")" + String(msg) + R"(","ts":)" + String(millis()) + "}");
// or with a struct:
struct Payload { const char* message; double ts; NOTE_FIELDS(message, ts) };
req.body(Payload{msg, (double)millis()});
```

Body conversion is complex — the script emits the struct approach with
a `// TODO(migrate): verify body struct` comment.

### Array construction
Input:
```cpp
J* files_array = JAddArrayToObject(req, "files");
for (int i = 0; i < file_count; i++) {
    JAddItemToArray(files_array, JCreateString(files[i]));
}
```

Output:
```cpp
for (int i = 0; i < file_count; i++) {
    req.files().add(files[i]);
}
```

### Error handling simplification
Input:
```cpp
J* rsp = notecard.requestAndResponse(req);
if (!rsp) {
    out.println("ERROR: no response");
    return false;
}
const char* err = JGetString(rsp, "err");
if (err && *err) {
    out.printf("ERROR: %s\n", err);
    JDelete(rsp);
    return false;
}
// ... use rsp ...
JDelete(rsp);
```

Output:
```cpp
auto r = nc.execute(req);  // or req.execute()
if (!r) {
    out.print("ERROR: ");
    out.println(r.error().message);
    return false;
}
// ... use r.field ...
```

## Tier 3 — Flag for Review (emit TODO comments)

- Polling loops with `requestAndResponse` inside (`while`/`for`)
- Nested `JGetObject` chains with fallback field names
- `JHasObjectItem` for distinguishing missing from false
- `J*` pointers passed across function boundaries
- `J*` returned from functions
- Complex ternary chains with `J*`
- String-based state machines (`strcmp` chains on response fields)

## Request Model

For each detected request, the script builds:

```python
RequestModel(
    endpoint="hub.set",           # from newRequest/newCommand
    is_command=False,             # newCommand vs newRequest
    fields=[
        Field(name="product", type="string", value="productUID",
              conditional=None),
        Field(name="mode", type="string", value='"continuous"',
              conditional="use_continuous_mode"),
        ...
    ],
    response_fields=[             # from JGetString/Int/Bool on the response
        ResponseField(name="version", type="string", var="ver"),
        ResponseField(name="device", type="string", var="dev"),
    ],
    has_body=True,
    body_fields=[...],
    error_handling="standard",    # null+err check pattern
)
```

## Test Strategy

### Ground truth: before/after pairs
- `~/e/notecard-tests` — the manual migration provides expected output
- Run migrate.py on the "before", diff against the "after"
- Track conversion rate: what % of lines are correctly transformed

### Incremental benchmark
- `~/e/firmware2/provisioning` — complex real-world project
- Run migrate.py, count Tier 1/2/3 annotations
- Target: Tier 1 handles 60%+ of J* call sites, Tier 2 handles 20%+

### Blues public examples
- `note-arduino` examples on GitHub
- `app-accelerators` projects
- These validate the tool works on code written by other developers

### Regression
- Converted output must compile (syntax check with `-fsyntax-only`)
- Converted output must produce identical wire format (test with mock transport)

## Implementation Phases

1. **J* flow graph**: track pointer declarations, assignments, and uses
   within a function. Handle standalone + init-statement forms.
2. **Request model builder**: extract endpoint, fields, and conditionals
   from the flow graph.
3. **Emitter**: generate note-cpp fluent chains from the model.
4. **Response model**: extract JGet* calls on response pointers, map to
   typed field access.
5. **Body handling**: detect JCreateObject+JAdd*+JAddItemToObject pattern.
6. **Tier 3 flagging**: detect complex patterns and emit TODO comments.
7. **Agent mode**: optionally pipe Tier 3 patterns to an LLM for conversion.
