# Custom Field Transforms

> **Status:** Design proposal, partly subsumed by the shipped `NOTE_STRICT_BODY_FIELDS` mechanism (see [`strict-body-fields.md`](strict-body-fields.md)). Kept here as historical design context for the field-transform problem space across `note-cpp`, `embedded-config-cpp`, and `note-cpp-app`.

## Problem

All three libraries (note-cpp, embedded-config-cpp, note-cpp-app) convert
between user-defined C++ types and external representations (JSON, env var
strings). Today, only built-in types (bool, int, float, string) are
supported. Users with custom types (enums, compound types, domain objects)
must work around this limitation.

**Examples of what users want:**

```cpp
// Enum stored as a string in JSON/env vars
enum class LogLevel { Debug, Info, Warn, Error };

// Compound type stored as a JSON object in note bodies
struct GeoPoint { double lat; double lon; };

// Compact type stored as a delimited string in env vars
struct Color { uint8_t r, g, b; };  // "255,128,0"
```

## Current State

| Library | Write | Read | Custom extension |
|---------|-------|------|-----------------|
| **note-cpp** | `write_field()` — `if constexpr` chain | `read_field()` / `_note_read_field()` — `if constexpr` chain | None — unsupported types silently skipped |
| **embedded-config-cpp** | N/A (string-based providers) | `from_string<T>()` specialization | Partial — user specializes `ec::from_string<T>` |
| **note-cpp-app** | N/A (delegates to note-cpp) | `EnvParser<T>` specialization | Partial — user specializes `EnvParser<T>` |

embedded-config-cpp has the best model today — `from_string<T>` is a
clean customization point. But it only covers string → T, not T → string
or T → JSON or JSON → T.

## Design Goals

1. **One mechanism** across all three libraries for extending field conversion
2. **Non-invasive** — user doesn't modify library code or open library namespaces
3. **Compile-time detected** — missing transforms are compile errors, not silent skips
4. **Supports multiple representations** — JSON (note-cpp bodies) and strings (env vars)
5. **Zero overhead** for built-in types — the `if constexpr` fast path stays

## Approach: ADL + Fallback Traits

Two complementary mechanisms:

### 1. ADL (Argument-Dependent Lookup) — primary

The library calls unqualified functions. The compiler finds them via ADL
in the user's type's namespace:

```cpp
// User defines alongside their type:
namespace myapp {

struct GeoPoint { double lat; double lon; };

// JSON ↔ GeoPoint
void note_write(note::JsonBuilder& b, note::string_view name, const GeoPoint& v) {
    b.begin_object(name);
    b.add("lat", v.lat);
    b.add("lon", v.lon);
    b.end_object();
}

void note_read(const note::JsonReader& r, note::string_view name, GeoPoint& v) {
    auto obj = r.get_object(name);
    if (obj) {
        v.lat = obj->get_double("lat");
        v.lon = obj->get_double("lon");
    }
}

// String ↔ GeoPoint (for env vars / embedded-config-cpp)
void note_to_string(const GeoPoint& v, std::string& out) {
    out = std::to_string(v.lat) + "," + std::to_string(v.lon);
}

bool note_from_string(note::string_view raw, GeoPoint& v) {
    // parse "lat,lon"
    auto comma = raw.find(',');
    if (comma == raw.npos) return false;
    v.lat = std::stod(std::string(raw.substr(0, comma)));
    v.lon = std::stod(std::string(raw.substr(comma + 1)));
    return true;
}

} // namespace myapp
```

The library's `write_field` becomes:

```cpp
template<typename V>
void write_field(JsonBuilder& b, string_view name, const V& value) {
    if constexpr (std::is_same_v<V, bool>) {
        b.add(name, value);
    } else if constexpr (std::is_integral_v<V>) {
        b.add(name, static_cast<int32_t>(value));
    } else if constexpr (std::is_floating_point_v<V>) {
        b.add(name, static_cast<double>(value));
    } else if constexpr (std::is_convertible_v<V, string_view>) {
        b.add(name, string_view(value));
    } else if constexpr (ReflectableAggregate<V>) {
        b.begin_object(name);
        write_aggregate(value, b);
        b.end_object();
    } else if constexpr (has_note_write<V>) {
        // ADL finds note_write in the type's namespace
        note_write(b, name, value);
    } else {
        static_assert(always_false<V>, "No write_field support for this type. "
            "Define note_write(JsonBuilder&, string_view, const T&) in your type's namespace.");
    }
}
```

### 2. Traits — fallback for types you don't own

If the type is from a third-party library (can't add ADL functions to its
namespace), specialize a traits struct:

```cpp
// For a type you don't own:
template<>
struct note::FieldTraits<third_party::Quaternion> {
    static void write(JsonBuilder& b, string_view name, const third_party::Quaternion& v) {
        b.begin_object(name);
        b.add("w", v.w); b.add("x", v.x); b.add("y", v.y); b.add("z", v.z);
        b.end_object();
    }

    static void read(const JsonReader& r, string_view name, third_party::Quaternion& v) {
        auto obj = r.get_object(name);
        if (obj) { v.w = obj->get_double("w"); /* ... */ }
    }
};
```

The lookup order in `write_field`:
1. Built-in types (bool, int, float, string) — `if constexpr`
2. Reflectable aggregates — automatic field-by-field
3. ADL `note_write` — user-defined in type's namespace
4. `FieldTraits<T>` specialization — for third-party types
5. `static_assert` — compile error with helpful message

### Function signatures

**JSON transforms:**
```cpp
void note_write(note::JsonBuilder& b, note::string_view name, const T& value);
void note_read(const note::JsonReader& r, note::string_view name, T& out);
```

**String transforms (for env vars / config):**
```cpp
void note_to_string(const T& value, std::string& out);
bool note_from_string(note::string_view raw, T& out);  // returns false on parse error
```

**Template type hints (for Notecard templates):**
```cpp
void note_template_hint(note::JsonBuilder& b, note::string_view name, const T*);
// The T* is nullptr — only the type matters, not the value.
// Default: skip (field not included in template).
```

### Detection traits

```cpp
namespace note::detail {
    template<typename T, typename = void>
    struct has_note_write : std::false_type {};

    template<typename T>
    struct has_note_write<T, std::void_t<decltype(
        note_write(std::declval<JsonBuilder&>(),
                   std::declval<string_view>(),
                   std::declval<const T&>()))>>
        : std::true_type {};

    // Similar for has_note_read, has_note_from_string, etc.
}
```

## Cross-Library Consistency

All three libraries use the same function names:

| Function | Used by | Purpose |
|----------|---------|---------|
| `note_write(b, name, v)` | note-cpp body serialization | T → JSON |
| `note_read(r, name, v)` | note-cpp body parsing | JSON → T |
| `note_to_string(v, out)` | embedded-config-cpp schema_as_json | T → string |
| `note_from_string(raw, v)` | embedded-config-cpp field parsing, note-cpp-app EnvVar | string → T |
| `note_template_hint(b, name, (T*)nullptr)` | note-cpp template_of | T → Notecard type hint |

embedded-config-cpp's existing `ec::from_string<T>` would call through to
`note_from_string` via ADL, maintaining backward compatibility.

## Replacing existing customization points

None of these libraries are published — no backward compatibility needed.

**embedded-config-cpp:** Replace `ec::from_string<T>` with `note_from_string`
via ADL. Remove the `ec::from_string` specialization mechanism entirely.

**note-cpp-app:** Replace `EnvParser<T>` with `note_from_string` via ADL.
Remove the `EnvParser` specialization mechanism.

## Examples

### Enum (string representation)

```cpp
enum class SyncMode { Periodic, Continuous, Minimum, Off };

void note_write(note::JsonBuilder& b, note::string_view name, const SyncMode& v) {
    static constexpr const char* names[] = {"periodic", "continuous", "minimum", "off"};
    b.add(name, note::string_view(names[static_cast<int>(v)]));
}

void note_read(const note::JsonReader& r, note::string_view name, SyncMode& v) {
    auto s = r.get_string(name);
    if (s == "periodic")   v = SyncMode::Periodic;
    else if (s == "continuous") v = SyncMode::Continuous;
    // ...
}

bool note_from_string(note::string_view raw, SyncMode& v) {
    if (raw == "periodic")   { v = SyncMode::Periodic; return true; }
    else if (raw == "continuous") { v = SyncMode::Continuous; return true; }
    // ...
    return false;
}
```

### Nested object

```cpp
struct GeoPoint { double lat; double lon; };

void note_write(note::JsonBuilder& b, note::string_view name, const GeoPoint& v) {
    b.begin_object(name);
    b.add("lat", v.lat);
    b.add("lon", v.lon);
    b.end_object();
}

void note_read(const note::JsonReader& r, note::string_view name, GeoPoint& v) {
    auto obj = r.get_object(name);
    if (obj) {
        v.lat = obj->get_double("lat");
        v.lon = obj->get_double("lon");
    }
}

// As an env var: "42.577,-70.871"
bool note_from_string(note::string_view raw, GeoPoint& v) {
    auto comma = raw.find(',');
    if (comma == raw.npos) return false;
    v.lat = std::stod(std::string(raw.substr(0, comma)));
    v.lon = std::stod(std::string(raw.substr(comma + 1)));
    return true;
}
```

### Array field

```cpp
struct SensorArray { float values[8]; uint8_t count; };

void note_write(note::JsonBuilder& b, note::string_view name, const SensorArray& v) {
    b.begin_array(name);
    for (uint8_t i = 0; i < v.count; ++i) b.add_element(v.values[i]);
    b.end_array();
}
```

## Implementation Plan

1. Add `has_note_write` / `has_note_read` detection traits to note-cpp
2. Update `write_field` / `read_field` with ADL fallback and `static_assert`
3. Add `FieldTraits<T>` for third-party types
4. Replace `ec::from_string<T>` with `note_from_string` ADL in embedded-config-cpp
5. Replace `EnvParser<T>` with `note_from_string` ADL in note-cpp-app
6. Add tests: custom enum, nested object, array, third-party type
7. Document with examples in each library
