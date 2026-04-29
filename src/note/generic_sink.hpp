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

#include <note/body_handler.hpp>
#include <note/compiler.hpp>
#include <note/field.hpp>
#include <note/field_desc.hpp>
#include <note/json_sax.hpp>
#include <note/lexer/sax_adapter.hpp>
#include <note/string_pool.hpp>
#include <note/types.hpp>

namespace note {

/// Non-template sink — one instantiation for all response types.
/// Handles response-level fields via field descriptor table, and forwards
/// body events to a BodyHandler (GenericBodySink when NOTE_RESPONSE_BODY=0).
struct GenericResponseSink {
    void* rsp;
    const FieldDesc* fields;
    uint8_t n_fields;
    StringPool* pool;
    BodyHandler body_handler_{};
    int body_depth_ = 0;

    void set_body_handler(BodyHandler bh) { body_handler_ = bh; }

    void on_null(string_view k) {
        if (body_depth_ > 0 && body_handler_) body_handler_.send(BodyEvent::make_bool(k, false));
    }

    void on_bool(string_view k, bool v) {
        if (body_depth_ > 0) { if (body_handler_) body_handler_.send(BodyEvent::make_bool(k, v)); return; }
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (d.type == FieldType::Bool && detail::flash_key_eq(k, d.name)) {
                set_field<bool>(d.offset, v);
                return;
            }
        }
    }

    void on_int(string_view k, json_int_t v) {
        if (body_depth_ > 0) { if (body_handler_) body_handler_.send(BodyEvent::make_int(k, v)); return; }
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if ((d.type == FieldType::Int || d.type == FieldType::Int32) && detail::flash_key_eq(k, d.name)) {
                if (d.type == FieldType::Int) set_field<json_int_t>(d.offset, v);
                else set_field<int32_t>(d.offset, static_cast<int32_t>(v));
                return;
            }
        }
    }

    void on_float(string_view k, double v) {
        if (body_depth_ > 0) { if (body_handler_) body_handler_.send(BodyEvent::make_float(k, v)); return; }
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (d.type == FieldType::Double && detail::flash_key_eq(k, d.name)) {
                set_field<double>(d.offset, v);
                return;
            }
        }
    }

    void on_string(string_view k, string_view v) {
        if (body_depth_ > 0) { if (body_handler_) body_handler_.send(BodyEvent::make_string(k, v)); return; }
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (d.type == FieldType::String && detail::flash_key_eq(k, d.name)) {
                set_field<string_view>(d.offset, pool->intern(v));
                return;
            }
        }
    }

    void on_number(string_view k, string_view raw) {
        if (body_depth_ > 0 && body_handler_) body_handler_.send(BodyEvent::make_number(k, raw));
    }

    void on_object_begin(string_view k) {
        if (body_depth_ > 0) {
            ++body_depth_;
            if (body_handler_) body_handler_.send(BodyEvent::make_object_begin(k));
            return;
        }
        if (detail::flash_key_eq(k, detail::common_keys::body)) { body_depth_ = 1; return; }
    }

    void on_object_end(string_view k) {
        if (body_depth_ > 0) {
            --body_depth_;
            if (body_depth_ > 0 && body_handler_) body_handler_.send(BodyEvent::make_object_end(k));
            return;
        }
    }

    void on_array_begin(string_view k) {
        if (body_depth_ > 0 && body_handler_) body_handler_.send(BodyEvent::make_array_begin(k));
    }

    void on_array_end(string_view k) {
        if (body_depth_ > 0 && body_handler_) body_handler_.send(BodyEvent::make_array_end(k));
    }

    void reset() {
        body_depth_ = 0;
        if (body_handler_) body_handler_.send(BodyEvent::make_reset());
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            switch (d.type) {
            case FieldType::Bool:    reset_field<bool>(d.offset); break;
            case FieldType::Int8:    reset_field<int8_t>(d.offset); break;
            case FieldType::Int16:   reset_field<int16_t>(d.offset); break;
            case FieldType::Int32:  reset_field<int32_t>(d.offset); break;
            case FieldType::Int:   reset_field<json_int_t>(d.offset); break;
            case FieldType::Float32: reset_field<float>(d.offset); break;
            case FieldType::Double:  reset_field<double>(d.offset); break;
            case FieldType::String:  reset_field<string_view>(d.offset); break;
            }
        }
    }

private:
    template<typename T>
    T* field_ptr(uint16_t offset) {
        return reinterpret_cast<T*>(static_cast<char*>(rsp) + offset);
    }

    /// Assign a value to a response field at the given offset.
    /// Writes through ResponseField<T>::operator= so presence is tracked.
    template<typename T>
    void set_field(uint16_t offset, T v) {
        *reinterpret_cast<ResponseField<T>*>(static_cast<char*>(rsp) + offset) = v;
    }

    /// Reset a response field to its default.
    template<typename T>
    void reset_field(uint16_t offset) {
        auto* rf = reinterpret_cast<ResponseField<T>*>(static_cast<char*>(rsp) + offset);
        *rf = ResponseField<T>{};
    }
};

// Outlined dispatch helper for GenericResponseSink. Replaces the per-SinkT
// 10-way switch that make_sax_dispatch<T> would otherwise inline at the
// call site (~1.3 KB on AVR). The lambda installed by the make_sax_dispatch
// overload below is a thin forwarder; the switch body lives once here.
inline NOTE_SINK_NOINLINE void dispatch_sax_event(GenericResponseSink& s, const SaxEvent& ev) {
    switch (ev.tag) {
    case SaxEvent::Null:        s.on_null(ev.key); break;
    case SaxEvent::Bool:        s.on_bool(ev.key, ev.b); break;
    case SaxEvent::Int:         s.on_int(ev.key, ev.i); break;
    case SaxEvent::Float:       s.on_float(ev.key, ev.f); break;
    case SaxEvent::String:      s.on_string(ev.key, {ev.sv.data, ev.sv.len}); break;
    case SaxEvent::ObjectBegin: s.on_object_begin(ev.key); break;
    case SaxEvent::ObjectEnd:   s.on_object_end(ev.key); break;
    case SaxEvent::ArrayBegin:  s.on_array_begin(ev.key); break;
    case SaxEvent::ArrayEnd:    s.on_array_end(ev.key); break;
    case SaxEvent::Reset:       s.reset(); break;
    default: NOTE_UNREACHABLE();
    }
}

/// Specialised make_sax_dispatch for GenericResponseSink: routes through
/// the outlined dispatch_sax_event helper instead of inlining the switch
/// per-SinkT instantiation. Selected by overload resolution over the
/// templated make_sax_dispatch<T>.
inline SaxDispatch make_sax_dispatch(GenericResponseSink& s) {
    return SaxDispatch{
        &s,
        [](void* p, const SaxEvent& ev) {
            dispatch_sax_event(*static_cast<GenericResponseSink*>(p), ev);
        }
    };
}

/// Non-template body sink — table-driven dispatch for flat body structs.
/// Used when NOTE_RESPONSE_BODY=0 to avoid per-body-type StructSink instantiations.
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

    void on_int(string_view k, json_int_t v) {
        for (uint8_t i = 0; i < n_fields; ++i) {
            auto d = detail::read_field_desc(&fields[i]);
            if (detail::flash_key_eq(k, d.name)) {
                assign_numeric(d, static_cast<double>(v));
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
                    assign_int(d, parse_int(raw));
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
            case FieldType::Int32:  *field_ptr<int32_t>(d.offset) = 0; break;
            case FieldType::Int:    *field_ptr<json_int_t>(d.offset) = 0; break;
            case FieldType::Float32: *field_ptr<float>(d.offset) = 0.0f; break;
            case FieldType::Double:  *field_ptr<double>(d.offset) = 0.0; break;
            case FieldType::String:  *field_ptr<string_view>(d.offset) = {}; break;
            }
        }
    }

private:
    void assign_int(const FieldDesc& d, json_int_t v) {
        switch (d.type) {
        case FieldType::Int8:    *field_ptr<int8_t>(d.offset)    = static_cast<int8_t>(v); break;
        case FieldType::Int16:   *field_ptr<int16_t>(d.offset)   = static_cast<int16_t>(v); break;
        case FieldType::Int32:   *field_ptr<int32_t>(d.offset)   = static_cast<int32_t>(v); break;
        case FieldType::Int:     *field_ptr<json_int_t>(d.offset) = v; break;
        default: break;
        }
    }

    void assign_numeric(const FieldDesc& d, double v) {
        switch (d.type) {
        case FieldType::Int8:    *field_ptr<int8_t>(d.offset)  = static_cast<int8_t>(v); break;
        case FieldType::Int16:   *field_ptr<int16_t>(d.offset) = static_cast<int16_t>(v); break;
        case FieldType::Int32:  *field_ptr<int32_t>(d.offset) = static_cast<int32_t>(v); break;
        case FieldType::Int:    *field_ptr<json_int_t>(d.offset) = static_cast<json_int_t>(v); break;
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
