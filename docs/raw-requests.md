# Raw Requests

The typed API covers all the patterns for each Notecard request covered in the
official Blues Notecard API, defined by their [API schema](https://github.com/blues/notecard-schema).
But the Notecard firmware may support mode combinations, field values, or
new features that the typed API doesn't yet model. Raw requests let you
bypass the typed layer and send any JSON the Notecard understands.

## Three levels of escape

### 1. Raw string fields on typed requests

Every typed request has field setters that accept `string_view`. You can
pass any string — it goes directly to the wire with no validation:

```cpp
// Typed intent (validated):
nc.card.attn().arm().connected().motion().execute();

// Same request via raw string on the base Request type:
note::api::CardAttn::Request req;
req.mode = "arm,connected,motion,some_new_mode";
req.execute();
```

The base `Request` type exposes all fields without intent filtering.
This is useful when:
- A new firmware version adds a mode the typed API doesn't cover yet
- You need a field combination that spans multiple intents
- You're prototyping and don't want type safety yet

### 2. Ad-hoc requests via `Notecard::request()`

For endpoints or field combinations not in the generated types at all:

This works with both the streaming and buffered transports. Use it for entirely new request types or field combinations not yet in the generated API:

```cpp
auto result = nc.request("card.attn", [](note::JsonBuilder& b) {
    b.add("mode", "some-future-mode");
    b.add("seconds", 120);
});
if (result) {
    auto& reader = *result.value();
    auto set = reader.get_bool("set");
}
```

This bypasses the generated types entirely — you build JSON by hand
and parse the response manually. No type safety, but maximum flexibility.

### 3. Fire-and-forget commands

```cpp
nc.command("card.attn", [](note::JsonBuilder& b) {
    b.add("mode", "disarm,-all");
});
```

Same as `request()` but sends `"cmd"` instead of `"req"` — no response
expected.

## When to use each level

| Need | Use |
|------|-----|
| Standard operations | Typed intents (`nc.card.attn().arm()`) |
| Existing endpoint, unusual field combo | Raw string on `Request` type |
| New/unknown endpoint or field | `nc.request()` with builder lambda |
| Fire-and-forget | `nc.command()` with builder lambda |

## Validation at each level

| Level | Compile-time | Runtime |
|-------|-------------|---------|
| Typed intent + flag methods | Field existence, flag scoping | None needed |
| Typed intent + named constants | Named constant validity | None needed |
| Typed intent + string literal (C++20 GCC) | `consteval` flag validation | None needed |
| Raw string on Request | None | Notecard validates |
| `request()` / `command()` | None | Notecard validates |

The Notecard firmware always validates the request and returns an error
if a field or mode is invalid. The typed API catches mistakes earlier —
at compile time rather than on the device.
