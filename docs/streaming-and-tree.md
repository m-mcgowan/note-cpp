# Streaming and tree modes

`note-cpp` parses Notecard responses in one of two ways. In **streaming mode**, a SAX parser fires events directly into your typed response struct as the response bytes arrive over the wire; nothing is held in memory after the call returns. In **tree mode**, a `JsonBackend` builds the entire response into a walkable `JsonReader` that lives on the response object, so you can query its body fields by name after the call returns. The mode is decided once, at construction time, by which `Notecard` constructor you call.

For most of the API, this choice is invisible. The typed builders, the response field accessors, the struct-based body parsing, the raw JSON escape hatch, and the binary transfer path all behave identically in either mode. The handful of features that genuinely depend on which mode is active are listed in [Where the modes diverge](#where-the-modes-diverge) below.

## What works the same in both modes

The bulk of the typed API is mode-agnostic. Code written against any of these surfaces compiles and runs without change in either mode:

- **Typed request builders.** Both the fluent style (`nc.hub.set().product("...").mode("periodic").execute()`) and the direct-assignment style produce the same wire request and return the same typed `Response`.
- **Typed response fields.** Reading `r.version`, `r.device`, or any other named response field works the same way in both modes. The JSON layer populates these fields whether it built a tree or fired SAX events along the way.
- **Struct body parsing with `.into(T&)`.** Capturing the response body into a struct is a streaming-friendly idiom that also works in tree mode. There is no intermediate tree to walk, and there is no allocator pressure beyond the struct itself.
- **Struct body sending with `.body(struct)`.** Building a request body from a typed struct produces the same JSON in either mode.
- **Raw JSON passthrough.** The low-level escape hatches `nc.transact(json, buf)` and `nc.send(json)` do not depend on the JSON layer at all, so they are unaffected by the mode.
- **Binary transfers.** `card.binary.put()` and `card.binary.get()` use COBS framing rather than JSON and behave identically in either mode.
- **Error handling.** The `ApiResult` truthy operator, the `r.error()` accessor, and the structured `ErrorInfo` shape are mode-agnostic.

For a quick concrete example, the following snippet compiles and behaves the same way whether the `Notecard` was constructed in streaming mode or tree mode:

```cpp
auto r = nc.card.version().execute();
if (r) {
    log(r.version);
    log(r.device);
}
```

## Where the modes diverge

Tree mode keeps the parsed response in memory after the call returns. That property unlocks two patterns that streaming mode cannot support.

**Post-call body inspection.** `response.body()` returns a `JsonReader*` you can walk by key. This is useful when the body shape is dynamic, when you only know which fields to read at runtime, or when you are exploring an unfamiliar endpoint:

```cpp
auto r = nc.note.get("data.qi").execute();

// Tree mode — query the parsed JsonReader by key after the call:
if (r && r.body()) {
    double temp = r.body()->get_double("temperature");
    int    hum  = r.body()->get_int("humidity");
}
```

In streaming mode the response bytes are gone once the SAX parser has run, so `r.body()` returns null. The same use case is served by committing the body shape ahead of time, either with a struct via `.into(struct)` or with a SAX `JsonSink` for fully ad-hoc parsing:

```cpp
struct Readings {
    float temperature;
    int humidity;
    NOTE_FIELDS(temperature, humidity);
};

Readings readings{};
nc.note.get("data.qi").into(readings).execute();
```

When the body shape is known ahead of time, `.into(struct)` is the preferred idiom in either mode. It is faster than walking a tree, has lower memory cost, and works on the smallest targets where tree mode is not available at all.

**The lambda request builder.** The lambda form `nc.request("endpoint", [&](auto& b) { ... })` is tree-mode only, because it builds the request as an in-memory JSON tree before serializing it to the wire. Streaming mode does not link the builder primitives the lambda form depends on. The typed API and the raw `nc.transact()` escape hatch cover the same ground in streaming mode.

## Picking a backend

Streaming mode does not need a `JsonBackend` and does not link one. Tree mode requires a `JsonBackend` — the backend is what turns response bytes into the walkable tree, and what serializes outgoing requests from typed inputs.

```cpp
// Streaming mode — no backend, zero heap, smallest flash.
note::Notecard nc(transport, note::Allocator{});

// Tree mode (default backend) — cJSON-backed, heap-allocated nodes,
// familiar from note-c projects.
note::backends::CjsonBackend backend;
note::Notecard nc(backend, transport);
```

The library ships four tree-mode backends with different memory profiles:

- **`CjsonBackend`** allocates the cJSON tree from the heap. This is the most familiar option for projects coming from note-c, and the default suggestion for hosts where heap is plentiful.
- **`StaticJsonBackend<BufN, TokN>`** uses a fixed-size `char[BufN]` for request and response bytes and a fixed jsmn token array for parse output. Allocation is fully predictable, no heap is touched, and the worst-case footprint is visible at compile time.
- **`CjsonArenaBackend`** allocates the cJSON tree from a `MonotonicArena` that you supply, and reclaims it with `arena.reset()` between request cycles. You get cJSON's familiar walking API with no calls into `malloc`.
- **`NlohmannBackend`** uses nlohmann-json's tree on the heap. This is useful when other parts of your project already depend on nlohmann.

The detailed comparison, sizing guidance, and customization notes live in [json-backend.md](json-backend.md).

## Selection guide

**Pick tree mode when:**

- You are migrating from note-c and want the lambda request builder, which matches the familiar "build JSON, send, parse" pattern.
- The response body shape is dynamic or only known at runtime, so you need to walk a tree by key.
- You are debugging wire traffic and want a tree to inspect at runtime.
- You have heap or arena budget available and prefer the post-call walking API.

**Pick streaming mode when:**

- You are targeting a memory-constrained device (Cortex-M0, AVR, the smaller ESP32 variants) and want no tree in memory and no `JsonBackend` linked.
- All your body shapes are known at compile time, so `.into(T&)` is the natural fit.
- You do not need `response.body()` for any endpoint.
- You want the smallest possible flash footprint; tree-mode backends bring in additional code that streaming mode does not.

## Feature parity

This table summarizes the surfaces touched by the mode choice. Every other surface in the library is mode-agnostic.

| Feature | Tree mode | Streaming mode |
|---|:---:|:---:|
| Typed `execute()` on requests | yes | yes |
| Typed response fields | yes | yes |
| `.into(T&)` body parse into struct | yes | yes |
| `.body(struct)` send struct as body | yes | yes |
| `nc.transact(json, buf)` raw JSON | yes | yes |
| Binary transfers (COBS) | yes | yes |
| Error handling (`ApiResult`) | yes | yes |
| Lambda request builder (`nc.request(...)`) | yes | — |
| `response.body() -> JsonReader*` | yes | — |
| Requires `JsonBackend` | yes | no |
| Zero-heap capable | depends on backend | yes |

Defining `NOTE_NO_BUFFERED` removes tree mode entirely (a saving of roughly 2 to 4 KB of flash). `NOTE_MINIMAL` sets this automatically.

## See also

- [json-backend.md](json-backend.md) — backend selection in depth, including memory sizing and customization.
- [transport-serial.md](transport-serial.md) and [transport-i2c.md](transport-i2c.md) — the wire transports underneath the JSON layer.
- [memory.md](memory.md) — overall memory model, arena sizing, and the streaming vs tree trade-off at the allocation level.
- [working-with-responses.md](working-with-responses.md) — patterns for reading response data in both modes.
