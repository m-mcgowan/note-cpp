#pragma once

/// @file body_handler.hpp
/// Type-erased body event and handler — shared by StructSink and GenericResponseSink.

#include <note/types.hpp>

#include <cstddef>
#include <cstdint>

namespace note {

/// Type-erased body event — single dispatch avoids per-event-type thunks.
struct BodyEvent {
    enum Tag : uint8_t {
        Bool, Int, Float, String, Number,
        ObjectBegin, ObjectEnd, ArrayBegin, ArrayEnd, Reset
    };
    struct StringRef { const char* data; size_t len; };

    Tag tag;
    string_view key;
    union {
        bool b;
        json_int_t i;
        double f;
        StringRef sv;
    };

    static BodyEvent make_bool(string_view k, bool v) { BodyEvent e; e.tag = Bool; e.key = k; e.b = v; return e; }
    static BodyEvent make_int(string_view k, json_int_t v) { BodyEvent e; e.tag = Int; e.key = k; e.i = v; return e; }
    static BodyEvent make_float(string_view k, double v) { BodyEvent e; e.tag = Float; e.key = k; e.f = v; return e; }
    static BodyEvent make_string(string_view k, string_view v) { BodyEvent e; e.tag = String; e.key = k; e.sv = {v.data(), v.size()}; return e; }
    static BodyEvent make_number(string_view k, string_view v) { BodyEvent e; e.tag = Number; e.key = k; e.sv = {v.data(), v.size()}; return e; }
    static BodyEvent make_object_begin(string_view k) { BodyEvent e; e.tag = ObjectBegin; e.key = k; return e; }
    static BodyEvent make_object_end(string_view k) { BodyEvent e; e.tag = ObjectEnd; e.key = k; return e; }
    static BodyEvent make_array_begin(string_view k) { BodyEvent e; e.tag = ArrayBegin; e.key = k; return e; }
    static BodyEvent make_array_end(string_view k) { BodyEvent e; e.tag = ArrayEnd; e.key = k; return e; }
    static BodyEvent make_reset() { BodyEvent e; e.tag = Reset; return e; }
};

/// Type-erased body handler — single function pointer dispatch.
struct BodyHandler {
    void* ctx = nullptr;
    void (*dispatch)(void*, const BodyEvent&) = nullptr;

    void send(const BodyEvent& ev) const { dispatch(ctx, ev); }
    explicit operator bool() const { return ctx != nullptr; }
};

class StringPool;  // forward

/// Type-erased body handler factory — shared by generated endpoints.
using BodyHandlerFactory = BodyHandler(*)(void* body, StringPool& pool, void* storage);

} // namespace note
