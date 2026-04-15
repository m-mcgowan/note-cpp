# Intent-focused APIs

Some Notecard requests do very different things depending on which fields you send. `note.get` reads a Note when you only pass `file`; add `"delete":true` and it pops the Note off a queue instead. `card.location.mode` can query the current mode, configure periodic GPS, set a fixed location, or remove the mode — all via the same wire request.

On the wire, these are one endpoint each, and it's up to you to remember which fields go together. In `note-cpp` they're split into named **intents** — one method per behavior, each exposing only the fields that apply.

## A minimal example

Reading a Note vs. popping it:

```cpp
// Read a Note — non-destructive, safe to retry
auto r = api.note.read("data.qi").noteId("my-note").execute();

// Pop from a queue — destructive, removes the Note on success
auto r = api.note.pop("requests.qi").execute();
```

Same Notecard endpoint (`note.get`), two different intents, two different C++ methods. Autocomplete shows you the intents available on `api.note.*`, and each intent only exposes fields that make sense for that operation.

## Why this matters

### 1. Only the fields that apply

Intents expose the fields the Notecard actually uses for that operation. Fields that don't apply aren't on the type, so setting them is a compile error rather than a silent wire-level bug.

`card.location.mode` has several intents. `fixed()` takes `lat` and `lon`; the other intents don't:

```cpp
// Configure a fixed location — lat and lon available
api.card.location.mode.fixed()
    .lat(42.565).lon(-70.783)
    .execute();

// Query current mode — no lat/lon fields
auto r = api.card.location.mode.get().execute();

// api.card.location.mode.get().lat(42.565);   // compile error: no such field
```

With the raw Notecard API, setting `lat` on a query is silently ignored — you find out it didn't do what you meant only when behavior is wrong in production.

### 2. Retry safety is known at compile time

Each intent carries a `Safety` level: `ReadOnly`, `Idempotent`, `NonIdempotent`, or `Destructive`. This matters because when a request times out, the library and your application code need to know whether repeating it is safe.

For `note.get`:
- `read()` is `ReadOnly` — the Note stays in the queue, retry freely.
- `pop()` is `Destructive` — if the response is lost mid-flight, the Notecard may have already removed the Note. Retrying would skip the *next* one.

The library's retry logic uses this automatically. You can also check it yourself:

```cpp
auto req = api.note.pop("data.qi");
static_assert(decltype(req)::safety == Safety::Destructive);
```

See [error-handling.md](error-handling.md) for how safety levels interact with retry semantics.

### 3. Response shape matches the intent

Each intent has its own response struct. `fixed()` returns a confirmation; `get()` returns the current mode plus live fix data. You only see the fields that actually come back for the operation you called.

## Access patterns: property vs. factory method

Some intent groups are reached as a property (`.`) and some as a method call (`()`). The shape depends on whether the group has further nested groups:

```cpp
// Factory method — simple endpoints where you pick an intent
api.card.attn().arm("location,motion").execute();
api.note.read("data.qi").execute();

// Property access — groups that also have nested groups
api.card.location.mode.fixed().lat(42.565).lon(-70.783).execute();
api.card.binary.status().execute();
```

The IDE autocomplete will disambiguate: if you get a `CardAttnFactory` member instead of a call result, add parens.

## Intents you'll see most often

| Intent | Meaning |
|--------|---------|
| `read()`, `get()` | Non-destructive query |
| `set()`, `configure()` | Update config |
| `reset()`, `clear()`, `remove()`, `stop()` | Reset to default / clear stored data |
| `pop()` | Read *and* remove from a queue |
| `peek()` | Look at queued items without removing |
| `status()` | Operational status (not config) |
| `fixed()`, `periodic()`, `continuous()` | Mode-specific configuration (e.g. GPS) |
| `arm()`, `sleep()`, `disarm()`, `retrieve()`, `query()` | Lifecycle (e.g. `card.attn`) |

A few examples across endpoints:

```cpp
// card.location.mode — configure GPS
api.card.location.mode.periodic().seconds(300).execute();
api.card.location.mode.continuous().execute();
api.card.location.mode.get().execute();

// card.binary — binary store management
api.card.binary.status().execute();
api.card.binary.clear().execute();

// card.attn — interrupt-driven wake
api.card.attn().arm("location,motion").execute();
api.card.attn().disarm().execute();
api.card.attn().query().execute();

// note.templates — register a typed template
api.note.templates().define("sensors.qo").body(template_of<Readings>()).execute();
```

## Dropping back to the raw request

If you need to do something the intent API doesn't expose, you can always talk to the underlying Notecard request directly. See [raw-requests.md](raw-requests.md).

The tradeoff is that the raw request is just JSON — no compile-time field checking, no retry-safety classification, and you're responsible for knowing which fields produce which behavior.

## A note on older code

Before this pattern settled, some intents were named after their HTTP verb (`get()`, `set()`, `delete_()`). Those names still compile for backwards compatibility — they're marked `[[deprecated]]` and the warning text points you to the current name. For example:

```cpp
// Still compiles, but warns — use remove() instead:
api.card.location.mode.delete_().execute();
```
