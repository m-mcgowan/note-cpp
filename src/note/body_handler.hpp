#pragma once

/// @file body_handler.hpp
/// Type-erased body event and handler — shared by StructSink and GenericResponseSink.

#include <note/types.hpp>
#include <note/json_sax.hpp>

#include <cstddef>
#include <cstdint>
#include <new>

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

namespace detail {

/// Adapter held in body_storage when a request's body is wired to a JsonSink&.
/// Translates BodyEvent records to JsonSink::on_* virtual calls.
struct JsonSinkBodyAdapter {
    JsonSink* sink;
};

/// Single dispatch function — shared across every endpoint that takes a
/// JsonSink. The codegen-generated factory does no per-endpoint work other
/// than handing this function pointer to the BodyHandler.
inline void dispatch_jsonsink_body_event(void* ctx, const BodyEvent& ev) {
    auto* a = static_cast<JsonSinkBodyAdapter*>(ctx);
    switch (ev.tag) {
    case BodyEvent::Bool:        a->sink->on_bool(ev.key, ev.b); break;
    case BodyEvent::Int:         a->sink->on_int(ev.key, ev.i); break;
    case BodyEvent::Float:       a->sink->on_float(ev.key, ev.f); break;
    case BodyEvent::String:      a->sink->on_string(ev.key, {ev.sv.data, ev.sv.len}); break;
    case BodyEvent::Number:      a->sink->on_number(ev.key, {ev.sv.data, ev.sv.len}); break;
    case BodyEvent::ObjectBegin: a->sink->on_object_begin(ev.key); break;
    case BodyEvent::ObjectEnd:   a->sink->on_object_end(ev.key); break;
    case BodyEvent::ArrayBegin:  a->sink->on_array_begin(ev.key); break;
    case BodyEvent::ArrayEnd:    a->sink->on_array_end(ev.key); break;
    case BodyEvent::Reset:       a->sink->reset(); break;
    default: break;
    }
}

} // namespace detail

/// Single shared factory for the `req.into(JsonSink&)` overload. Codegen-
/// generated `into(JsonSink&)` stores `&jsonsink_body_factory` in
/// `body_handler_factory_` — no per-endpoint instantiation.
inline BodyHandler jsonsink_body_factory(void* body, StringPool& /*pool*/, void* storage) {
    auto* adapter = new (storage) detail::JsonSinkBodyAdapter{static_cast<JsonSink*>(body)};
    return {adapter, &detail::dispatch_jsonsink_body_event};
}

} // namespace note
