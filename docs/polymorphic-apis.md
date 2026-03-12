# Polymorphic APIs

Some Notecard API endpoints behave differently depending on which fields you send. In `note-c`, these are a single function — you pass different fields and hope you got the combination right. In `note-cpp`, each behavior is a distinct type with its own fields, response type, and safety level.

## The pattern

Polymorphic endpoints are generated as an outer struct containing named inner types:

```cpp
struct NoteGet {
    struct Get { ... };     // Read a Note (Safety::ReadOnly)
    struct Delete { ... };  // Pop a Note from queue (Safety::Destructive)
};
```

Access via the `Api` object:

```cpp
// Read a Note — non-destructive
auto r = api.noteGet().get().file("data.qi").execute();

// Pop from queue — destructive, sends "delete":true
auto r = api.noteGet().delete_().file("requests.qi").execute();
```

Each variant has its own:
- **Fields** — only the fields relevant to that behavior
- **Response type** — the response struct matches what the Notecard returns for that variant
- **Safety level** — `ReadOnly`, `Idempotent`, or `Destructive`, informing retry decisions

## Full list of polymorphic endpoints

| Endpoint | Variants | Notes |
|----------|----------|-------|
| `card.binary` | `Get`, `Delete` | Get reads binary data; Delete clears the buffer |
| `card.contact` | `Get`, `Set` | Get reads contact info; Set updates it |
| `card.location.mode` | `Get`, `Set`, `Delete` | Get reads GPS mode; Set configures it; Delete resets |
| `card.power` | `Get`, `Set`, `Delete` | Get reads power config; Set changes it; Delete resets |
| `card.temp` | `Get`, `Set`, `Delete` | Get reads temperature; Set starts periodic logging; Delete stops it |
| `card.voltage` | `Get`, `Set` | Get reads voltage; Set configures monitoring |
| `card.wireless.penalty` | `Get`, `Set`, `Delete` | Wireless penalty management |
| `env.default` | `Set`, `Delete` | Set creates env vars; Delete removes them |
| `note.changes` | `Get`, `Delete` | Get lists changes; Delete acknowledges (advances tracker) |
| `note.get` | `Get`, `Delete` | Get reads a Note; Delete pops it from a queue |
| `note.template` | `Set`, `Delete` | Set registers a template; Delete removes it |

## Why this matters

### 1. Only the fields that apply

Each variant exposes only the fields the Notecard expects for that operation. Fields that don't apply simply don't exist on the type — setting them is a compile error, not a silent wire-level mistake.

`card.location.mode` is a clear example. The `Set` variant has `lat` and `lon` fields (for configuring a fixed location). The `Get` and `Delete` variants don't — those fields make no sense when querying or resetting:

```cpp
// Set — lat and lon are available
api.cardLocationMode().set()
    .mode("fixed").lat(42.565).lon(-70.783)
    .execute();

// Get — no lat/lon fields; this won't compile:
// api.cardLocationMode().get().lat(42.565);  // error: no member named 'lat'

// Delete — same, just resets the mode
api.cardLocationMode().delete_().execute();
```

In `note-c`, all three operations go through the same function. Nothing stops you from setting `lat` on a query — it's just ignored, making bugs hard to spot.

Similarly, `card.temp::Delete` doesn't have a `stop` field (it hardcodes `"stop":true` internally), while `Get` and `Set` do — because stop only makes sense as an explicit choice when you're not already requesting deletion.

### 2. Safety classification

Each variant carries a compile-time safety level that tells your retry logic whether repeating the operation is safe:

```cpp
static_assert(NoteGet::Get::safety == Safety::ReadOnly);      // always safe to retry
static_assert(NoteGet::Delete::safety == Safety::Destructive); // retrying may skip a Note
```

Consider `note.get`: **Get** is `ReadOnly` — if the transport fails mid-response, retry freely. **Delete** pops the Note from the queue — if the response is lost, retrying would skip the next Note. With `note-c`, both operations go through the same function with no compile-time distinction.

### 3. Response types

Each variant can have a different response structure matching what the Notecard actually returns for that operation. The type system ensures you only access fields that exist in the response for the variant you called.

## Three-variant endpoints

Some endpoints have three behaviors. `card.location.mode`:

```cpp
// Query current GPS mode (ReadOnly)
auto r = api.cardLocationMode().get().execute();

// Set fixed location — has lat, lon, mode fields (Idempotent)
api.cardLocationMode().set()
    .mode("fixed")
    .lat(42.565)
    .lon(-70.783)
    .execute();

// Reset to default mode (Destructive)
api.cardLocationMode().delete_().execute();
```
