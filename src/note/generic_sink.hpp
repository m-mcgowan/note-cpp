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

#include <note/field_desc.hpp>
#include <note/json_sax.hpp>
#include <note/string_pool.hpp>
#include <note/types.hpp>

namespace note {

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
            case FieldType::Bool:    *field_ptr<bool>(d.offset) = false; break;
            case FieldType::Int8:    *field_ptr<int8_t>(d.offset) = 0; break;
            case FieldType::Int16:   *field_ptr<int16_t>(d.offset) = 0; break;
            case FieldType::Int32:   *field_ptr<int32_t>(d.offset) = 0; break;
            case FieldType::Float32: *field_ptr<float>(d.offset) = 0.0f; break;
            case FieldType::Double:  *field_ptr<double>(d.offset) = 0.0; break;
            case FieldType::String:  *field_ptr<string_view>(d.offset) = {}; break;
            }
        }
    }

private:
    template<typename T>
    T* field_ptr(uint16_t offset) {
        return reinterpret_cast<T*>(static_cast<char*>(rsp) + offset);
    }
};

/// Non-template body sink — table-driven dispatch for flat body structs.
/// Used under NOTE_MINIMAL to avoid per-body-type StructSink instantiations.
/// Flat structs only: no nested objects or arrays.
struct GenericBodySink {
    void* obj;
    const FieldDesc* fields;
    uint8_t n_fields;
    StringPool* pool;

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
            if (detail::flash_key_eq(k, d.name)) {
                assign_numeric(d, v);
                return;
            }
        }
    }

    void on_float(string_view k, double v) {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (detail::flash_key_eq(k, d.name)) {
                assign_numeric(d, v);
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

    void on_number(string_view k, string_view raw) {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (detail::flash_key_eq(k, d.name)) {
                if (d.type == FieldType::Float32 || d.type == FieldType::Double)
                    assign_numeric(d, parse_double(raw));
                else
                    assign_numeric(d, static_cast<double>(parse_int(raw)));
                return;
            }
        }
    }

    void on_object_begin(string_view) {}
    void on_object_end(string_view) {}
    void on_array_begin(string_view) {}
    void on_array_end(string_view) {}

    void reset() {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            switch (d.type) {
            case FieldType::Bool:    *field_ptr<bool>(d.offset) = false; break;
            case FieldType::Int8:    *field_ptr<int8_t>(d.offset) = 0; break;
            case FieldType::Int16:   *field_ptr<int16_t>(d.offset) = 0; break;
            case FieldType::Int32:   *field_ptr<int32_t>(d.offset) = 0; break;
            case FieldType::Float32: *field_ptr<float>(d.offset) = 0.0f; break;
            case FieldType::Double:  *field_ptr<double>(d.offset) = 0.0; break;
            case FieldType::String:  *field_ptr<string_view>(d.offset) = {}; break;
            }
        }
    }

private:
    void assign_numeric(const FieldDesc& d, double v) {
        switch (d.type) {
        case FieldType::Int8:    *field_ptr<int8_t>(d.offset)  = static_cast<int8_t>(v); break;
        case FieldType::Int16:   *field_ptr<int16_t>(d.offset) = static_cast<int16_t>(v); break;
        case FieldType::Int32:   *field_ptr<int32_t>(d.offset) = static_cast<int32_t>(v); break;
        case FieldType::Float32: *field_ptr<float>(d.offset)   = static_cast<float>(v); break;
        case FieldType::Double:  *field_ptr<double>(d.offset)  = v; break;
        default: break;
        }
    }

    template<typename T>
    T* field_ptr(uint16_t offset) {
        return reinterpret_cast<T*>(static_cast<char*>(obj) + offset);
    }
};

} // namespace note
