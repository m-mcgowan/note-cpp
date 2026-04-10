#pragma once

/// @file sax_adapter.hpp
/// SaxAdapter — bridges JsonLexer events to the JsonSink interface.
///
/// Accumulates KeyChar events into a key buffer and StringChar events
/// into a value buffer, then delivers complete (key, value) pairs to
/// a JsonSink. Buffer sizes are developer-controlled.
///
/// The adapter is NOT templated on the sink type. Instead, it dispatches
/// completed events through a function pointer to the concrete sink.
/// This ensures a single Lexer::feed<SaxAdapter> instantiation regardless
/// of how many sink types are used — critical for flash savings on AVR.

#include <note/json_sax.hpp>
#include <note/json_sax_streaming.hpp>  // SaxStreamBuf
#include <note/lexer/event.hpp>
#include <note/types.hpp>

#include <cstdint>

namespace note {

/// Tagged SAX event — single-dispatch alternative to per-event function pointers.
/// Structurally similar to BodyEvent but for the lexer->sink path.
/// Has Null (no Number) because the lexer parses numbers into int/float.
struct SaxEvent {
    enum Tag : uint8_t {
        Null, Bool, Int, Float, String,
        ObjectBegin, ObjectEnd, ArrayBegin, ArrayEnd, Reset
    };
    struct StringRef { const char* data; size_t len; };

    Tag tag;
    string_view key;
    union {
        bool b;
        int32_t i;
        double f;
        StringRef sv;
    };

    static SaxEvent make_null(string_view k) { SaxEvent e; e.tag = Null; e.key = k; return e; }
    static SaxEvent make_bool(string_view k, bool v) { SaxEvent e; e.tag = Bool; e.key = k; e.b = v; return e; }
    static SaxEvent make_int(string_view k, int32_t v) { SaxEvent e; e.tag = Int; e.key = k; e.i = v; return e; }
    static SaxEvent make_float(string_view k, double v) { SaxEvent e; e.tag = Float; e.key = k; e.f = v; return e; }
    static SaxEvent make_string(string_view k, string_view v) { SaxEvent e; e.tag = String; e.key = k; e.sv = {v.data(), v.size()}; return e; }
    static SaxEvent make_object_begin(string_view k) { SaxEvent e; e.tag = ObjectBegin; e.key = k; return e; }
    static SaxEvent make_object_end(string_view k) { SaxEvent e; e.tag = ObjectEnd; e.key = k; return e; }
    static SaxEvent make_array_begin(string_view k) { SaxEvent e; e.tag = ArrayBegin; e.key = k; return e; }
    static SaxEvent make_array_end(string_view k) { SaxEvent e; e.tag = ArrayEnd; e.key = k; return e; }
    static SaxEvent make_reset() { SaxEvent e; e.tag = Reset; return e; }
};

/// Type-erased event dispatcher — single function pointer dispatch.
/// Reduced from 11 members (void* + 10 fn ptrs) to 2 (void* + 1 fn ptr).
struct SaxDispatch {
    void* sink;
    void (*dispatch)(void*, const SaxEvent&);
};

/// Create a SaxDispatch that forwards events to a concrete SinkT.
/// Single dispatch function with switch — replaces 10 per-event thunks.
template<typename SinkT>
SaxDispatch make_sax_dispatch(SinkT& s) {
    return SaxDispatch{
        &s,
        [](void* p, const SaxEvent& ev) {
            auto& sink = *static_cast<SinkT*>(p);
            switch (ev.tag) {
            case SaxEvent::Null:        sink.on_null(ev.key); break;
            case SaxEvent::Bool:        sink.on_bool(ev.key, ev.b); break;
            case SaxEvent::Int:         sink.on_int(ev.key, ev.i); break;
            case SaxEvent::Float:       sink.on_float(ev.key, ev.f); break;
            case SaxEvent::String:      sink.on_string(ev.key, {ev.sv.data, ev.sv.len}); break;
            case SaxEvent::ObjectBegin: sink.on_object_begin(ev.key); break;
            case SaxEvent::ObjectEnd:   sink.on_object_end(ev.key); break;
            case SaxEvent::ArrayBegin:  sink.on_array_begin(ev.key); break;
            case SaxEvent::ArrayEnd:    sink.on_array_end(ev.key); break;
            case SaxEvent::Reset:       sink.reset(); break;
            }
        },
    };
}

/// Non-template SaxAdapter — single instantiation for all sink types.
class SaxAdapter {
public:
    SaxAdapter(SaxStreamBuf& buf, SaxDispatch dispatch)
        : dispatch_(dispatch)
        , key_buf_(buf.key)
        , key_cap_(buf.key_size)
        , val_buf_(buf.val)
        , val_cap_(buf.val_size)
    {}

    /// Process one lexer event. Callable as handler for JsonLexer::feed().
    void operator()(const LexerEvent& ev) { on_event(ev); }

    /// Process one lexer event. Call this from the lexer callback.
    void on_event(const LexerEvent& ev) {
        switch (ev.tag) {
        case LexerEvent::ObjectBegin:
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_object_begin(current_key()));
            push_key();
            break;
        case LexerEvent::ObjectEnd:
            pop_key();
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_object_end(current_key()));
            break;
        case LexerEvent::ArrayBegin:
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_array_begin(current_key()));
            in_array_depth_++;
            break;
        case LexerEvent::ArrayEnd:
            if (in_array_depth_ > 0) in_array_depth_--;
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_array_end(current_key()));
            break;
        case LexerEvent::KeyChar:
            if (!in_key_) { key_len_ = 0; in_key_ = true; }
            if (key_len_ < key_cap_) key_buf_[key_len_++] = ev.ch;
            break;
        case LexerEvent::KeyEnd:
            in_key_ = false;
            break;
        case LexerEvent::StringChar:
            if (val_len_ < val_cap_) val_buf_[val_len_++] = ev.ch;
            break;
        case LexerEvent::StringEnd:
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_string(current_key(),
                    string_view(val_buf_, val_len_)));
            val_len_ = 0;
            break;
        case LexerEvent::Integer:
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_int(current_key(), ev.integer));
            break;
        case LexerEvent::Float:
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_float(current_key(), ev.floating));
            break;
        case LexerEvent::Bool:
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_bool(current_key(), ev.boolean));
            break;
        case LexerEvent::Null:
            dispatch_.dispatch(dispatch_.sink,
                SaxEvent::make_null(current_key()));
            break;
        case LexerEvent::Error:
            error_ = ev.error;
            break;
        }
    }

    const char* error() const { return error_; }

    void reset() {
        key_len_ = 0;
        val_len_ = 0;
        stack_depth_ = 0;
        in_array_depth_ = 0;
        error_ = nullptr;
        dispatch_.dispatch(dispatch_.sink, SaxEvent::make_reset());
    }

private:
    string_view current_key() const {
        return string_view(key_buf_, key_len_);
    }

    void push_key() {
        if (stack_depth_ < kMaxDepth) {
            uint8_t save_len = key_len_ < kMaxKeyPerLevel
                ? static_cast<uint8_t>(key_len_) : kMaxKeyPerLevel;
            for (uint8_t i = 0; i < save_len; ++i)
                key_stack_[stack_depth_].saved[i] = key_buf_[i];
            key_stack_[stack_depth_].length = save_len;
            ++stack_depth_;
        }
        key_len_ = 0;
    }

    void pop_key() {
        if (stack_depth_ > 0) {
            --stack_depth_;
            uint8_t restore_len = key_stack_[stack_depth_].length;
            for (uint8_t i = 0; i < restore_len && i < key_cap_; ++i)
                key_buf_[i] = key_stack_[stack_depth_].saved[i];
            key_len_ = restore_len < key_cap_ ? restore_len : key_cap_;
        } else {
            key_len_ = 0;
        }
    }

    SaxDispatch dispatch_;

    char* key_buf_;
    size_t key_cap_;
    size_t key_len_ = 0;

    char* val_buf_;
    size_t val_cap_;
    size_t val_len_ = 0;

    static constexpr uint8_t kMaxDepth = 8;
    static constexpr uint8_t kMaxKeyPerLevel = 32;
    struct KeySlot {
        char saved[kMaxKeyPerLevel]{};
        uint8_t offset = 0;
        uint8_t length = 0;
    };
    KeySlot key_stack_[kMaxDepth]{};
    uint8_t stack_depth_ = 0;
    uint8_t in_array_depth_ = 0;
    bool in_key_ = false;

    const char* error_ = nullptr;
};

} // namespace note
