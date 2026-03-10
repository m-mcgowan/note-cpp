# note-app Design

A higher-level library sitting above note-cpp. Provides stateful, app-centric
abstractions for common Notecard usage patterns.

## Implementation Status

| Component | Design | Impl | Tests |
|-----------|--------|------|-------|
| `DirectChannel` | Done | Done | Done |
| `StaticStateStore` / `NullStateStore` | Done | Done | Done |
| `TemplateManager` | Done | - | - |
| `SyncManager` | Done | - | - |
| `AttentionManager` | Done | - | - |
| `ConnectionManager` | Done | - | - |
| `NotePublisher` | Done | - | - |
| `ConfigManager` Phase 1 | Done | - | - |
| `ConfigManager` Phase 2 | Partial | - | - |
| `QueuedChannel` / `TickChannel` | Done | - | - |
| Composites / Procedures | Done | - | - |
| `DfuManager` | Outline | - | - |

The sections below describe the full vision. See the table above for current
state.

```
┌─────────────────────────────────────────────┐
│  User application                           │
│    reads StateStore, calls managers         │
├──────────────────┬──────────────────────────┤
│  Managers        │  StateStore              │
│  AttentionManager│  typed, observable       │
│  ConfigManager   │  projection of Notecard  │
│  SyncManager     │  state; shared between   │
│  etc.            │  app and managers        │
├──────────────────┴──────────────────────────┤
│  INoteChannel                               │
│  DirectChannel / QueuedChannel / TickChannel│
│  composites, procedures, queue, re-entrancy │
├─────────────────────────────────────────────┤
│  note-cpp                                   │
│  Generated API, Notecard coordinator        │
│  Transport: CRC, retry, segmentation        │
├──────────────────┬──────────────────────────┤
│  note-arduino-cpp│  (other platform HALs)   │
└──────────────────┴──────────────────────────┘
```

---

## StateStore

The StateStore is a first-class shared domain model — a typed, observable
local projection of Notecard state. It serves two purposes:

1. **App access**: the app reads state directly for monitoring and decision
   making without making Notecard requests.
2. **Write optimisation**: managers read from the store before writes,
   avoiding redundant round-trips.

The store is injected into both app components and managers. The channel
stays narrow — it knows nothing about state.

```cpp
class StateStore {
public:
    // Read current state. Returns nullopt if not yet populated.
    template<typename T>
    std::optional<T> get() const;

    // Update state (called by managers after successful Notecard responses).
    template<typename T>
    void set(T value);

    // Discard state — next get() returns nullopt, next manager read hits Notecard.
    template<typename T>
    void invalidate();

    // Observe changes. Callback fired on every set<T>().
    template<typename T>
    void on_change(std::function<void(const T&)> callback);
};
```

### Usage

```cpp
StateStore store;
AttentionManager attn(channel, store);
ConfigManager<AppConfig> config(channel, store);

// App reads directly — no Notecard round-trip.
if (auto s = store.get<AttentionState>()) {
    display_sources(s->sources);
}

// React to changes.
store.on_change<AppConfig>([](const AppConfig& cfg) {
    apply_config(cfg);   // fired whenever ConfigManager reloads
});

// Manager writes update the store; observers fire automatically.
config.reload();
attn.arm(AttnSource::Files);
```

### Implementations

| Implementation | Behaviour |
|---|---|
| `InMemoryStateStore` | Heap-allocated map — default for RTOS / host |
| `StaticStateStore<Types...>` | Fixed type list, no heap — embedded / bare-metal |
| User-defined | Flash-backed, shared-memory, test fixture, etc. |

---

## Channel interface

All app components depend on `INoteChannel` — never on note-cpp directly.
The channel is I/O dispatch only: queuing, thread safety, execution strategy.
State management is handled by the StateStore, not the channel.

```cpp
class INoteChannel {
public:
    virtual ~INoteChannel() = default;

    // Synchronous: blocks until response (or timeout).
    template<typename Req>
    ApiResult<typename Req::Response> execute(Req req,
        uint32_t    timeout_ms   = kDefaultTimeoutMs,
        bool        priority     = false,
        const char* coalesce_key = nullptr);

    // Asynchronous with callback.
    template<typename Req>
    bool submit(Req req,
        std::function<void(ApiResult<typename Req::Response>)> callback,
        uint32_t    timeout_ms   = kDefaultTimeoutMs,
        bool        priority     = false,
        const char* coalesce_key = nullptr);

    // Fire-and-forget.
    template<typename Req>
    bool submit(Req req,
        uint32_t    timeout_ms   = kDefaultTimeoutMs,
        bool        priority     = false,
        const char* coalesce_key = nullptr);

    // Cooperative tick — no-op on QueuedChannel, drives the queue on TickChannel.
    virtual void tick() {}
};
```

### Global channel access

Allows runtime swap from direct to queued without changing app code:

```cpp
INoteChannel* note_channel();
void          note_channel_set(INoteChannel*);
```

---

## Channel implementations

### `DirectChannel`

Executes requests immediately, holding an optional mutex. No queue.
Suitable for single-threaded or bare-metal use where blocking is acceptable.

### `QueuedChannel`

Active-object pattern: `std::deque` + `std::mutex` + `std::condition_variable`
+ worker thread. Requests from other threads enqueue and block on a future.
Suitable for RTOS / `std::thread` environments.

### `TickChannel`

Cooperative: `std::deque`, no worker thread. One entry processed per `tick()`
call. Suitable for bare-metal Arduino `loop()`.

```cpp
void loop() {
    note_channel()->tick();
    // ... rest of loop
}
```

---

## Queue entry

The queue is type-erased at the callable level, not the string level. The
request object is captured by value and JSON is generated at execution time,
not at enqueue time. This supports composites and procedures that generate
request strings dynamically based on runtime state or intermediate responses.

```cpp
struct Entry {
    std::function<void(INoteChannel&)> run;  // captures request + callback
    uint32_t    timeout_ms;
    bool        priority;
    const char* coalesce_key;
};
```

`submit<Req>()` builds the lambda at enqueue time, closing over the typed
request object and callback. JSON generation happens inside `run()`.

---

## Re-entrancy: automatic tick handling

When a composite or procedure calls back into the channel from within `run()`,
the channel detects re-entrancy and executes the inner request directly against
the transport — no re-queuing, no extra tick required.

- `TickChannel`: tracks `executing_` flag; re-entrant calls bypass the queue.
- `QueuedChannel`: checks `is_worker_thread()`; re-entrant calls execute inline.
- `DirectChannel`: trivially re-entrant (no queue).

This means a composite or procedure runs to completion within a single `tick()`
or worker dispatch, regardless of how many inner requests it makes.

---

## Request types: single, composite, procedure

All three satisfy the same `Request` concept — an object with
`execute(INoteChannel&)` — and use the same `channel.execute()` /
`channel.submit()` call site.

### Single request

A generated note-cpp endpoint type. One JSON round-trip.

```cpp
channel.execute(api::CardVersion{});
```

### Composite

A named type encapsulating a fixed, non-conditional sequence of requests.
Reusable, testable in isolation with a mock channel.

```cpp
struct AttentionArm {
    using Response = CardAttnResponse;
    AttnSourceSet sources;

    ApiResult<Response> execute(INoteChannel& ch) const {
        return ch.execute(api::CardAttn{}.mode("arm").sources(sources));
    }
};

channel.execute(AttentionArm{.sources = AttnSource::Files});
```

### Procedure

A closure with conditional logic. Useful for ad-hoc multi-step operations
where the next step depends on the result of the previous one. No rollback
implied — "procedure" is the correct name (not "transaction").

```cpp
auto op = note::procedure([&](INoteChannel& ch) -> ApiResult<void> {
    auto current = ch.execute(api::CardAttn{});
    if (!current) return current.error();
    auto merged = current.sources | AttnSource::Files;
    if (merged == current.sources) return {};   // no-op
    return ch.execute(api::CardAttn{}.mode("arm").sources(merged));
});

channel.execute(op);
channel.submit(op, callback);
```

Composites and procedures are not mutually exclusive. Composites cover the
common case cleanly. Procedures cover cases that require conditional logic.

---

## Planned app components

All managers take `INoteChannel&` and `StateStore&`. They read from the store
before Notecard reads (avoiding redundant round-trips), write to the store
after successful responses, and fire store observers automatically.

### `AttentionManager`

Manages the attention pin and source set. The Notecard requires the full
source list on every arm — no incremental update. The manager maintains the
desired set, reads current state from the store (or Notecard on first use),
diffs, and re-arms only when needed.

```cpp
attn.enable(AttnSource::Files);
attn.disable(AttnSource::Motion);
attn.arm();
bool fired = attn.triggered();      // polled
attn.on_trigger([]{ ... });         // callback / ISR-safe flag
auto why = attn.query_and_rearm();

// App can also read attention state directly from store:
auto s = store.get<AttentionState>();
```

### `ConfigManager<T>`

Binds a typed config struct to Notecard environment variables via a declarative
schema. Handles change detection, bulk load, type conversion, validation,
derivation, and a three-layer priority system. Writes the resolved config to
the StateStore; app observes changes reactively.

#### Three-layer resolution

Config fields are resolved in priority order — higher layers override lower ones:

```
Override  (runtime, in-memory — highest priority)
    ↓
Notecard  (Notehub env.get)
    ↓
Default   (schema-defined — lowest priority)
```

After all layers are merged, `derive()` runs to compute dependent fields, then
validators run, then handlers fire.

#### ValueSource

Each field carries a source tag indicating where its value came from:

```cpp
enum class ValueSource {
    Default,      // no layer set this field
    Environment,  // Notecard/Notehub provided it
    Override,     // runtime override set it
    Derived,      // computed from other fields in derive()
    Invalid,      // parse/validation failed, reverted to default
};
```

#### Schema declaration

```cpp
struct AppConfig {
    Seconds  idle_interval  = 60_s;
    Seconds  rfid_expire    = 0_s;   // derived if unset
    bool     developer_mode = false;
    uint8_t  led_brightness = 100;
    RGBA     error_color    = COLOR_ERROR;
};

template<>
struct note::EnvSchema<AppConfig> {
    static constexpr auto fields = std::tuple{
        note::field("idle interval",   &AppConfig::idle_interval)
            .unit<Seconds>()           // "3 mins", "90s", "1.5hr", "180" all accepted
            .range(0_s, 24_hr),

        note::field("rfid expire",     &AppConfig::rfid_expire)
            .unit<Seconds>(),

        note::field("developer mode",  &AppConfig::developer_mode),

        note::field("led brightness",  &AppConfig::led_brightness)
            .range(0, 100),

        note::field("error rgba",      &AppConfig::error_color),
    };

    // Post-merge derivation — runs after Default/Notecard/Override are resolved.
    // SourceMap lets derive() check whether a field was explicitly set.
    static void derive(AppConfig& cfg, note::SourceMap& sources) {
        if (sources.is_default(&AppConfig::rfid_expire)) {
            cfg.rfid_expire = cfg.idle_interval * 3 / 2;
            sources.mark_derived(&AppConfig::rfid_expire);
        }
    }
};
```

#### Sub-group schemas

Large configs are composed from sub-schemas. Cross-field validation lives in
the sub-schema's `validate()` hook:

```cpp
struct BatteryConfig {
    BatteryPercent critical = 10;
    BatteryPercent low      = 20;
    BatteryPercent full     = 95;
};

template<>
struct note::EnvSchema<BatteryConfig> {
    static constexpr auto fields = std::tuple{
        note::field("battery critical percent", &BatteryConfig::critical),
        note::field("battery low percent",      &BatteryConfig::low),
        note::field("battery full percent",     &BatteryConfig::full),
    };

    static void validate(BatteryConfig& cfg, note::ValidationContext& ctx) {
        if (cfg.critical > cfg.low) {
            ctx.warn("battery critical >= low, swapping");
            std::swap(cfg.critical, cfg.low);
        }
    }
};

struct AppConfig {
    BatteryConfig battery;
    Seconds       idle_interval = 60_s;
    // ...
};

template<>
struct note::EnvSchema<AppConfig> {
    static constexpr auto fields = std::tuple{
        note::sub_schema<BatteryConfig>(&AppConfig::battery),
        note::field("idle interval", &AppConfig::idle_interval).unit<Seconds>(),
    };
};
```

#### Validators — composable, common ones provided

```cpp
note::field("led brightness", &AppConfig::led_brightness)
    .validate(note::range(0, 100))
    .validate(note::divisible_by(5))           // custom: must be 0,5,10,...,100

note::field("mode", &AppConfig::mode)
    .validate(note::one_of("active", "passive", "off"))

note::field("threshold", &AppConfig::threshold)
    .validate([](float v) { return v > 0.0f && v < 1.0f; })
```

On failure: value reverts to default, `ValueSource::Invalid` recorded,
error handler called.

#### Handlers — three levels

```cpp
note::ConfigManager<AppConfig> config(channel, store, overrides);

// Field level — fires when that specific field's resolved value changes.
config.on_change(&AppConfig::idle_interval, [](Seconds v, ValueSource src) {
    reschedule_timer(v);
});

// Struct level — fires when any field changes; receives full resolved config.
// Also fires on initial load (subscribe_and_call semantics).
config.on_update([](const AppConfig& cfg, const note::ChangeSet& changes) {
    if (changes.contains(&AppConfig::idle_interval))
        reschedule_timer(cfg.idle_interval);
});

// Error handler — called for parse/validation failures.
config.on_error([](std::string_view name, std::string_view raw,
                   std::string_view reason) {
    log.printf("env: %s='%s' invalid: %s (using default)\n", name, raw, reason);
});
```

`on_update` fires on both initial load and subsequent changes (equivalent to
elert-notecard's `subscribe_and_call`). `on_change` fires only when the
resolved value differs from the previous resolved value.

#### Runtime overrides

```cpp
note::OverrideStore<AppConfig> overrides;

// Set a temporary override — re-runs resolve/derive/validate/notify immediately.
config.override(&AppConfig::developer_mode, true);
config.override(&AppConfig::idle_interval,  5_s);

// Clear individual override — field falls back to Notecard/Default.
config.clear_override(&AppConfig::developer_mode);

// Clear all overrides.
config.clear_overrides();
```

Overrides are in-memory (volatile). Persistence across sleep/reboot is the
app's responsibility (e.g. RTC memory, flash) — re-apply overrides on warm
boot before the first `load()`.

#### env.template — automatic, opt-out

Since `EnvSchema` already knows every field's C++ type, `ConfigManager`
sends `env.template` on startup automatically. This is a no-op on cellular
Notecards (minor optimisation) and required on LoRa/Starnote. Suppress with
`config.options().skip_env_template()` for old firmware targets.

#### Startup sequence

```
1. env.template   { body: <type hints derived from EnvSchema field types> }
2. env.default    { body: <default values from EnvSchema> }
3. env.get        → raw env values
4. resolve()      merge Default ← Notecard ← Override, record ValueSource per field
5. derive()       compute derived fields, update SourceMap
6. validate()     per-field validators + sub-schema validate() hooks
7. notify         on_update (always) + on_change (changed fields only)
```

Polling for changes:

```cpp
// In loop() or a periodic task:
if (config.poll_modified()) {
    // env.modified timestamp changed → re-runs steps 3-7
    // on_change fires only for fields whose resolved value actually changed
}
```

#### Units

The field's declared unit is the fallback when the env string has no suffix.
When the string includes a unit suffix it takes precedence:

```
"idle interval" = "3 mins"   → 3 minutes  (string suffix wins)
"idle interval" = "90"       → 90 seconds (field's .unit<Seconds>() applies)
"idle interval" = "1.5hr"    → 1.5 hours  (string suffix wins)
```

Common suffixes recognised: `ms`, `s`, `sec`, `min`, `mins`, `hr`, `h`.

### `SyncManager`

Trigger sync, wait for completion, handle status polling and timeout.
Supports `max_age` (don't sync if recently synced) and force mode.
Writes `HubSyncStatus` to the store after each sync.

### Others (future)

- Hub connectivity monitor (wait for connected/disconnected)
- DFU orchestration (multi-step: check, download, apply, verify)
- Note queue (batch adds, flush on sync)
- Binary transfers (`card.binary.get/put` chunking)

---

## Relationship to elert-notecard

elert-notecard (a prior implementation using note-c / J* pointers) informed
this design. Key differences:

| | elert-notecard | note-app |
|---|---|---|
| Request representation | `J*` (cJSON, type-erased at enqueue) | Typed object (type-erased at execution) |
| JSON generation | At enqueue time | At execution time (lazy) |
| Multi-step ops | Not supported | Composite + procedure |
| State management | Ad-hoc, per-function | Shared StateStore, observable |
| API style | Free functions + global state | Channel + StateStore injected |
| Platform | ESP32 / FreeRTOS | Platform-neutral |
