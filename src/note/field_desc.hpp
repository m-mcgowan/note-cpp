#pragma once

/// @file field_desc.hpp
/// Field descriptor infrastructure for table-driven SAX dispatch.
///
/// FieldType and FieldDesc are the shared primitives used by both
/// GenericResponseSink (response parsing) and GenericBodySink (body
/// parsing). Extracted here so body.hpp can generate field tables
/// in NOTE_FIELDS without pulling in the full SAX/pool stack.

#include <note/progmem.hpp>
#include <note/types.hpp>

#include <cstdint>
#include <type_traits>

namespace note {

/// Field type discriminator — determines how bytes are written to the field.
/// GenericResponseSink uses only Bool/Int32/Double/String (response fields are
/// always wide types). GenericBodySink uses all values (user body structs may
/// have narrower embedded-friendly types like float, int16_t).
enum class FieldType : uint8_t {
    Bool,
    Int8,
    Int16,
    Int32,    // int32_t — body struct fields, unit types
    Int,      // json_int_t — API response/request integer fields
    Float32,
    Double,
    String,
};

/// Describes one struct field: its JSON key, byte offset in the struct,
/// and value type. Stored in PROGMEM on AVR.
struct FieldDesc {
    const char* name;     // JSON key (PROGMEM pointer on AVR)
    uint16_t offset;      // byte offset within the struct
    FieldType type;
};

/// Map a C++ field type to its FieldType discriminator.
/// Sized: int8/16/32, float/double. Unsized integral → Int32, FP → Double.
template<typename T>
constexpr FieldType field_type_of() {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<V, bool>)
        return FieldType::Bool;
    else if constexpr (std::is_same_v<V, int8_t> || std::is_same_v<V, uint8_t>)
        return FieldType::Int8;
    else if constexpr (std::is_same_v<V, int16_t> || std::is_same_v<V, uint16_t>)
        return FieldType::Int16;
    else if constexpr (std::is_same_v<V, float>)
        return FieldType::Float32;
    else if constexpr (std::is_same_v<V, int32_t> || std::is_same_v<V, uint32_t>)
        return FieldType::Int32;
    else if constexpr (std::is_integral_v<V>)
        return FieldType::Int;
    else if constexpr (std::is_floating_point_v<V>)
        return FieldType::Double;
    else
        return FieldType::String;
}

namespace detail {

/// Read a FieldDesc from program memory (PROGMEM on AVR, direct on other platforms).
inline FieldDesc read_field_desc(const FieldDesc* p) {
#if NOTE_PROGMEM
    FieldDesc d;
    memcpy_P(&d, p, sizeof(d));
    return d;
#else
    return *p;
#endif
}

/// Compare a RAM string_view against a possibly-PROGMEM C string.
inline bool flash_key_eq(string_view k, const char* flash_name) {
#if NOTE_PROGMEM
    size_t len = strlen_P(flash_name);
    return k.size() == len && memcmp_P(k.data(), flash_name, len) == 0;
#else
    return k == flash_name;
#endif
}

} // namespace detail
} // namespace note
