#pragma once

/// @file generic_sink.hpp
/// GenericResponseSink — table-driven SAX sink for response parsing.
///
/// A single, non-template sink type that dispatches SAX events to struct
/// fields using a compile-time field descriptor table. Because it's one
/// concrete type, make_sax_dispatch<GenericResponseSink> generates exactly
/// ONE set of dispatch thunks shared by all response types — eliminating
/// the per-endpoint thunk duplication that dominates flash on AVR.
///
/// Field tables use NOTE_FLASH_ATTR (PROGMEM on AVR) so they live in flash
/// rather than RAM. Reads go through pgm_read helpers on Harvard platforms.

#include <note/json_sax.hpp>
#include <note/progmem.hpp>
#include <note/string_pool.hpp>
#include <note/types.hpp>

#include <cstdint>

namespace note {

/// Field type discriminator — matches the SAX event that delivers the value.
enum class FieldType : uint8_t {
    Bool,
    Int32,
    Double,
    String,
};

/// Describes one response field: its JSON key, byte offset in the response
/// struct, and value type. Stored in PROGMEM on AVR.
struct FieldDesc {
    const char* name;     // JSON key (PROGMEM pointer on AVR)
    uint16_t offset;      // byte offset within the response struct
    FieldType type;
};

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

/// Non-template sink — one instantiation for all response types.
struct GenericResponseSink {
    void* rsp;
    const FieldDesc* fields;
    uint8_t n_fields;
    StringPool* pool;

    void on_null(string_view) {}

    void on_bool(string_view k, bool v) {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (d.type == FieldType::Bool && detail::flash_key_eq(k, d.name)) {
                *field_ptr<bool>(d.offset) = v;
                return;
            }
        }
    }

    void on_int(string_view k, int32_t v) {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (d.type == FieldType::Int32 && detail::flash_key_eq(k, d.name)) {
                *field_ptr<int32_t>(d.offset) = v;
                return;
            }
        }
    }

    void on_float(string_view k, double v) {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (d.type == FieldType::Double && detail::flash_key_eq(k, d.name)) {
                *field_ptr<double>(d.offset) = v;
                return;
            }
        }
    }

    void on_string(string_view k, string_view v) {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (d.type == FieldType::String && detail::flash_key_eq(k, d.name)) {
                *field_ptr<string_view>(d.offset) = pool->intern(v);
                return;
            }
        }
    }

    void on_number(string_view, string_view) {}
    void on_object_begin(string_view) {}
    void on_object_end(string_view) {}
    void on_array_begin(string_view) {}
    void on_array_end(string_view) {}

    void reset() {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            switch (d.type) {
            case FieldType::Bool:   *field_ptr<bool>(d.offset) = false; break;
            case FieldType::Int32:  *field_ptr<int32_t>(d.offset) = 0; break;
            case FieldType::Double: *field_ptr<double>(d.offset) = 0.0; break;
            case FieldType::String: *field_ptr<string_view>(d.offset) = {}; break;
            }
        }
    }

private:
    template<typename T>
    T* field_ptr(uint16_t offset) {
        return reinterpret_cast<T*>(static_cast<char*>(rsp) + offset);
    }
};

} // namespace note
