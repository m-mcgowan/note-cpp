# Why note-cpp?

The Notecard C API works, but every request is a bag of untyped strings and numbers. Typos compile fine and fail at runtime. There's nothing to auto-complete.

<table>
<tr><th>note-c</th><th>note-cpp</th></tr>
<tr><td>

```c
// Configure product — field names are
// strings, types are manual, no IDE help.
J *req = NoteNewRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
NoteRequest(req);
```

</td><td>

```cpp
// Every field is a named member.
// IDE auto-completes after the dot.
api.hubSet()
   .product("com.example.app")
   .mode("periodic")
   .outbound(60)
   .execute();
```

</td></tr>
<tr><td>

```c
// Send a note — body is manual J* tree.
J *req = NoteNewRequest("note.add");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", 22.5);
JAddNumberToObject(body, "humidity", 60);
NoteRequest(req);
```

</td><td>

```cpp
// Body from a typed struct — same type
// registers templates and parses responses.
Readings r{.temperature = 22.5f, .humidity = 60};
api.noteAdd()
   .file("sensors.qo")
   .body(r)
   .execute();
```

</td></tr>
<tr><td>

```c
// Read response — stringly-typed, no
// compiler help if you misspell a field.
J *rsp = NoteRequestResponse(
    NoteNewRequest("card.version"));
char *ver = JGetString(rsp, "verison"); // typo!
char *dev = JGetString(rsp, "device");
NoteDeleteResponse(rsp);
```

</td><td>

```cpp
// Response is a typed struct — misspelled
// fields won't compile. Dot access, not arrow.
auto r = api.cardVersion().execute();
if (r) {
    auto ver = r.version; // typo = compile error
    auto dev = r.device;
}
```

</td></tr>
<tr><td>

```c
// Register template — magic numbers
// for type hints, easy to get wrong.
J *req = NoteNewRequest("note.template");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temperature", 14.1);
JAddNumberToObject(body, "humidity", 11);
NoteRequest(req);
```

</td><td>

```cpp
// Same Readings struct auto-generates
// the correct Notecard type hints.
api.noteTemplate().set("sensors.qo")
   .body(note::template_of<Readings>())
   .execute();
```

</td></tr>
</table>

With note-cpp, the compiler catches what note-c defers to runtime: wrong field names, wrong types, wrong enum values, missing required fields. And your IDE auto-completes every request, every field, and every response member.
