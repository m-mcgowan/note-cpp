# Static Memory Sizing — Compile-Time Arena Calculation

> **Note**: The `BodyCaptureSink` migration discussed below was superseded by
> [Streaming Body Parse](streaming-body-parse.md), which eliminates body
> buffering entirely. Body primitives now have zero arena cost.

## Goal

When all request types are known at compile time, note-cpp should be able
to statically determine the exact memory required for the response arena.
This eliminates heap allocation entirely — the developer declares a
fixed-size buffer and note-cpp guarantees it's sufficient for their
usage. Essential for RAM-restricted targets (AVR, Cortex-M0) where
`malloc` is unavailable or undesirable.

### Developer experience

```cpp
// Declare which requests this firmware uses
using MyRequests = note::RequestSet<
    note::api::CardStatus,
    note::api::HubSet,
    note::api::NoteAdd,
    note::api::NoteGet::Read
>;

// Compile-time: exact arena size for the worst-case response
static constexpr size_t kArenaSize = MyRequests::max_arena_size;

// Zero-heap notecard
uint8_t arena_buf[kArenaSize];
note::MonotonicArena arena(arena_buf);
note::StaticNotecard<SerialTransport> nc(transport, arena);
```

If the developer accidentally uses a request type not in `MyRequests`,
the compile fails (or a `static_assert` fires).

## What the arena stores

During a streaming response parse, the arena holds:

1. **Interned strings** — `StringPool::intern(v)` copies string values
   from the SAX parser's transient scratch buffer into the arena. Each
   interned string is `len + 1` bytes (null terminator).

2. **Body JSON capture** — `BodyCaptureSink` accumulates the body
   sub-object as a raw JSON string (currently via `std::string`, but
   should move to arena allocation).

The arena is per-transaction: created fresh for each `execute()` call
(or per-attempt in the retry loop). After the response is consumed,
the arena can be reused.

## What needs to be computed

For each response type `Rsp`:

- **String field budget**: sum of max expected lengths for each
  `ResponseField<string_view>` field. The Notecard protocol doesn't
  formally specify max lengths, but practical bounds exist:
  - `version`: ~40 chars (`"notecard-10.1.1.17591"`)
  - `status`: ~80 chars (e.g. `"idle {connected}"`)
  - `device`: ~25 chars (`"dev:860322068097069"`)
  - `err`: ~64 chars
  - `payload`: variable (base64-encoded, can be large for `dfu.get`)

- **Body JSON budget**: the serialized size of the body object. For
  typed bodies (`NOTE_FIELDS` structs), this is calculable from the
  field types and names.

- **Overhead**: null terminators, alignment padding.

The arena size is: `max(budget(Rsp) for Rsp in MyRequests)`.

## Approaches

### Approach 1: Explicit per-field max lengths in the spec

Add `x-max-length` to string properties in the OpenAPI spec. Codegen
emits a `constexpr size_t max_arena_size` per response type.

```cpp
// Generated
struct CardStatus::Response {
    static constexpr size_t max_arena_size =
        40 +   // version
        80 +   // status
        25 +   // device
        64 +   // err (always reserved)
        16;    // overhead
};
```

**Pros**: exact, verifiable at compile time, no runtime surprises.
**Cons**: requires maintaining max lengths in the spec; some fields
(like `dfu.get` payload) have unbounded lengths.

### Approach 2: Developer-declared budgets

The developer specifies a budget per request type:

```cpp
using MyRequests = note::RequestSet<
    note::WithBudget<note::api::CardStatus, 256>,
    note::WithBudget<note::api::NoteGet::Read, 1024>
>;
static constexpr size_t kArenaSize = MyRequests::max_budget;
```

**Pros**: simple, developer controls the trade-off.
**Cons**: manual, not automatically validated.

### Approach 3: Hybrid — spec defaults + developer overrides

Codegen provides default budgets from spec metadata. Developer can
override for specific types. A `static_assert` warns if the override
is smaller than the spec default.

**Recommended approach** — gives safe defaults with escape hatches.

### Approach 4: Body sizing from struct reflection

For `.into(T&)` where `T` has `NOTE_FIELDS`, the body JSON size is
calculable at compile time:

```cpp
struct SensorData {
    float temperature;    // {"temperature":  → 16 chars key + max float ~20
    int32_t humidity;     // {"humidity":     → 12 chars key + max int ~11
    NOTE_FIELDS(temperature, humidity)
};
// Max body JSON: {"temperature":-1.7976931e+308,"humidity":-2147483648}
// = ~60 bytes
```

With C++20 reflection, this is automatic. With `NOTE_FIELDS`, the macro
can emit a `constexpr` sizing function.

## Key code areas

An implementer needs to understand these files and concepts:

### Arena allocation

- **`include/note/arena.hpp`** — `MonotonicArena`: bump allocator over
  a fixed buffer. Used by `StaticNotecard`.
- **`include/note/allocator.hpp`** — `Allocator`: type-erased allocator
  interface. `StringPool` allocates through this.
- **`include/note/string_pool.hpp`** — `StringPool`: interns strings
  via `Allocator`. The main consumer of arena memory during parse.

### Response types and sinks

- **`include/note/api/*.hpp`** (generated) — each endpoint's `Response`
  struct and `Sink`. The sink calls `pool_.intern(v_)` for string fields.
  This is where arena memory is consumed.
- **`tools/codegen/templates/endpoint.hpp.j2`** — the Jinja template
  that generates response types and sinks. Any `constexpr` sizing would
  be added here.
- **`include/note/json_sax.hpp`** — `BodyCaptureSink`: accumulates body
  JSON. Currently uses `std::string` (heap). Needs to be changed to use
  the arena for static allocation.

### StaticNotecard

- **`include/note/static_notecard.hpp`** — `StaticNotecard<Transport>`:
  the zero-vtable, zero-heap notecard. Uses `MonotonicArena` instead of
  heap `Allocator`. This is the primary consumer of the static sizing.

### Streaming parser

- **`include/note/lexer/`** — the new zero-buffer lexer. The
  `SaxAdapter` uses a `SaxStreamBuf` for key/value accumulation —
  this buffer is also part of the static memory budget but is
  stack-allocated (not arena).
- **`include/note/streaming_transport.hpp`** — `receive_streaming()`
  drives the lexer. The `frame_read` and lookahead buffer are
  transport-level, not per-response.

### Binary transfer

- **`include/note/notecard.hpp`** — `do_binary_send/receive`: the
  binary pipeline uses a 256-byte `binary_ctrl_buf_` for control
  command responses. This is also part of the static budget.

## Compile-time validation tests

### Compile-fail tests (should NOT compile)

These go in `tests/compile_fail/`:

```cpp
// compile_fail/static_arena_too_small.cpp
// Verifies that a static_assert fires when the arena is too small.
#include <note/static_notecard.hpp>
#include <note/api/card_status.hpp>

uint8_t buf[4];  // way too small
note::MonotonicArena arena(buf);
// static_assert should fire in execute() or arena construction
```

```cpp
// compile_fail/request_not_in_set.cpp
// Verifies that using a request type not in the declared set fails.
using MyRequests = note::RequestSet<note::api::CardStatus>;
// Attempting to execute CardVersion (not in set) should fail:
// nc.execute(note::api::CardVersion{});  → static_assert
```

### Compile-check tests (should compile)

These go in `tests/compile_check/`:

```cpp
// compile_check/static_arena_sufficient.cpp
// Verifies that the computed arena size compiles and is reasonable.
using MyRequests = note::RequestSet<
    note::api::CardStatus,
    note::api::CardVersion,
    note::api::HubSet
>;
static_assert(MyRequests::max_arena_size > 0);
static_assert(MyRequests::max_arena_size < 4096);  // sanity bound

uint8_t buf[MyRequests::max_arena_size];
note::MonotonicArena arena(buf);
// Should compile without warnings.
```

### Runtime tests

```cpp
// test_static_sizing.cpp
TEST_CASE("static arena: CardStatus response fits in computed size") {
    constexpr size_t sz = note::api::CardStatus::Response::max_arena_size;
    uint8_t buf[sz];
    note::MonotonicArena arena(buf);
    // ... execute card.status with a mock transport ...
    // Verify no arena overflow
    REQUIRE(arena.bytes_used() <= sz);
}

TEST_CASE("static arena: max over request set") {
    using Requests = note::RequestSet<
        note::api::CardStatus,
        note::api::CardVersion
    >;
    constexpr size_t sz = Requests::max_arena_size;
    // Should be the max of the two
    static_assert(sz >= note::api::CardStatus::Response::max_arena_size);
    static_assert(sz >= note::api::CardVersion::Response::max_arena_size);
}
```

## BodyCaptureSink migration

The current `BodyCaptureSink` uses `std::string` for accumulation —
this is a heap allocation that defeats static sizing. Two options:

1. **Arena-backed accumulation**: `BodyCaptureSink` takes an arena
   reference and allocates from it. The body JSON size counts toward
   the arena budget.

2. **Streaming body parse**: instead of capturing raw JSON and
   re-parsing, the body is parsed incrementally by a nested sink.
   This eliminates body buffering entirely but requires the body
   type to be known at parse time (template parameter on the sink).
   This aligns with the lexer v2 direction.

Option 2 is the better long-term design but requires codegen changes.
Option 1 is a stepping stone.

## Open questions

- Should the arena size include padding for alignment? `MonotonicArena`
  currently doesn't align allocations.
- How to handle `dfu.get` and other endpoints with unbounded payloads?
  These may need a separate large buffer or streaming delivery.
- Should `RequestSet` be enforced at compile time (static_assert in
  execute) or just advisory (constexpr sizing helper)?
