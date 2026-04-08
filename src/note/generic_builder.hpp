#pragma once

/// @file generic_builder.hpp
/// GenericRequestBuilder — table-driven request serialization.
///
/// A single shared function that serializes request fields to JSON using
/// a field descriptor table. Replaces per-type build() methods with data,
/// reducing per-endpoint flash cost from ~414 bytes to ~30 bytes (table only).
///
/// Field descriptors use PROGMEM on AVR for zero RAM cost.

#include <note/json.hpp>
#include <note/progmem.hpp>
#include <note/types.hpp>

#include <cstdint>
#include <optional>

namespace note {

/// Field type for request serialization.
enum class ReqFieldType : uint8_t {
    Bool,
    Int32,
    Double,
    String,
};

/// Describes one request field: key name, byte offset, and type.
struct ReqFieldDesc {
    const char* name;     // PROGMEM key string
    uint16_t offset;      // byte offset of RequestField<T> in the request struct
    ReqFieldType type;
};

namespace detail {

inline ReqFieldDesc read_req_field(const ReqFieldDesc* p) {
#if NOTE_PROGMEM
    ReqFieldDesc d;
    memcpy_P(&d, p, sizeof(d));
    return d;
#else
    return *p;
#endif
}

} // namespace detail

/// Serialize request fields from a descriptor table into a JsonBuilder.
/// Checks each optional field — only set fields are serialized.
inline void generic_build(JsonBuilder& b, const void* req,
                          const ReqFieldDesc* fields, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i) {
        auto d = detail::read_req_field(&fields[i]);
        const auto* base = static_cast<const char*>(req) + d.offset;

        switch (d.type) {
        case ReqFieldType::Bool: {
            const auto& f = *reinterpret_cast<const std::optional<bool>*>(base);
            if (f) {
                FlashString key{d.name, 0};
#if NOTE_PROGMEM
                key.len = strlen_P(d.name);
#else
                key.len = __builtin_strlen(d.name);
#endif
                char kbuf[32];
                b.add(key.to_view(kbuf), *f);
            }
            break;
        }
        case ReqFieldType::Int32: {
            const auto& f = *reinterpret_cast<const std::optional<int32_t>*>(base);
            if (f) {
                FlashString key{d.name, 0};
#if NOTE_PROGMEM
                key.len = strlen_P(d.name);
#else
                key.len = __builtin_strlen(d.name);
#endif
                char kbuf[32];
                b.add(key.to_view(kbuf), *f);
            }
            break;
        }
        case ReqFieldType::Double: {
            const auto& f = *reinterpret_cast<const std::optional<double>*>(base);
            if (f) {
                FlashString key{d.name, 0};
#if NOTE_PROGMEM
                key.len = strlen_P(d.name);
#else
                key.len = __builtin_strlen(d.name);
#endif
                char kbuf[32];
                b.add(key.to_view(kbuf), *f);
            }
            break;
        }
        case ReqFieldType::String: {
            const auto& f = *reinterpret_cast<const std::optional<string_view>*>(base);
            if (f) {
                FlashString key{d.name, 0};
#if NOTE_PROGMEM
                key.len = strlen_P(d.name);
#else
                key.len = __builtin_strlen(d.name);
#endif
                char kbuf[32];
                b.add(key.to_view(kbuf), *f);
            }
            break;
        }
        }
    }
}

} // namespace note
