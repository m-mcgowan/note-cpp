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

/// Type-erased event dispatcher — routes completed SAX events to a concrete sink.
struct SaxDispatch {
    void* sink;
    void (*on_null)(void*, string_view);
    void (*on_bool)(void*, string_view, bool);
    void (*on_int)(void*, string_view, int32_t);
    void (*on_float)(void*, string_view, double);
    void (*on_string)(void*, string_view, string_view);
    void (*on_object_begin)(void*, string_view);
    void (*on_object_end)(void*, string_view);
    void (*on_array_begin)(void*, string_view);
    void (*on_array_end)(void*, string_view);
    void (*reset)(void*);
};

/// Dispatch thunk functions — noinline to prevent LTO from inlining
/// the entire sink method into each thunk. Keeps thunks tiny (~10 bytes)
/// while sink methods remain as separate, shareable functions.
namespace dispatch_thunks {
template<typename SinkT> __attribute__((noinline))
void on_null(void* p, string_view k) { static_cast<SinkT*>(p)->on_null(k); }
template<typename SinkT> __attribute__((noinline))
void on_bool(void* p, string_view k, bool v) { static_cast<SinkT*>(p)->on_bool(k, v); }
template<typename SinkT> __attribute__((noinline))
void on_int(void* p, string_view k, int32_t v) { static_cast<SinkT*>(p)->on_int(k, v); }
template<typename SinkT> __attribute__((noinline))
void on_float(void* p, string_view k, double v) { static_cast<SinkT*>(p)->on_float(k, v); }
template<typename SinkT> __attribute__((noinline))
void on_string(void* p, string_view k, string_view v) { static_cast<SinkT*>(p)->on_string(k, v); }
template<typename SinkT> __attribute__((noinline))
void on_object_begin(void* p, string_view k) { static_cast<SinkT*>(p)->on_object_begin(k); }
template<typename SinkT> __attribute__((noinline))
void on_object_end(void* p, string_view k) { static_cast<SinkT*>(p)->on_object_end(k); }
template<typename SinkT> __attribute__((noinline))
void on_array_begin(void* p, string_view k) { static_cast<SinkT*>(p)->on_array_begin(k); }
template<typename SinkT> __attribute__((noinline))
void on_array_end(void* p, string_view k) { static_cast<SinkT*>(p)->on_array_end(k); }
template<typename SinkT> __attribute__((noinline))
void do_reset(void* p) { static_cast<SinkT*>(p)->reset(); }
} // namespace dispatch_thunks

/// Create a SaxDispatch that forwards to a concrete SinkT.
template<typename SinkT>
SaxDispatch make_sax_dispatch(SinkT& s) {
    return SaxDispatch{
        &s,
        dispatch_thunks::on_null<SinkT>,
        dispatch_thunks::on_bool<SinkT>,
        dispatch_thunks::on_int<SinkT>,
        dispatch_thunks::on_float<SinkT>,
        dispatch_thunks::on_string<SinkT>,
        dispatch_thunks::on_object_begin<SinkT>,
        dispatch_thunks::on_object_end<SinkT>,
        dispatch_thunks::on_array_begin<SinkT>,
        dispatch_thunks::on_array_end<SinkT>,
        dispatch_thunks::do_reset<SinkT>,
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
            dispatch_.on_object_begin(dispatch_.sink, current_key());
            push_key();
            break;
        case LexerEvent::ObjectEnd:
            pop_key();
            dispatch_.on_object_end(dispatch_.sink, current_key());
            break;
        case LexerEvent::ArrayBegin:
            dispatch_.on_array_begin(dispatch_.sink, current_key());
            in_array_depth_++;
            break;
        case LexerEvent::ArrayEnd:
            if (in_array_depth_ > 0) in_array_depth_--;
            dispatch_.on_array_end(dispatch_.sink, current_key());
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
            dispatch_.on_string(dispatch_.sink, current_key(),
                                string_view(val_buf_, val_len_));
            val_len_ = 0;
            break;
        case LexerEvent::Integer:
            dispatch_.on_int(dispatch_.sink, current_key(), ev.integer);
            break;
        case LexerEvent::Float:
            dispatch_.on_float(dispatch_.sink, current_key(), ev.floating);
            break;
        case LexerEvent::Bool:
            dispatch_.on_bool(dispatch_.sink, current_key(), ev.boolean);
            break;
        case LexerEvent::Null:
            dispatch_.on_null(dispatch_.sink, current_key());
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
        dispatch_.reset(dispatch_.sink);
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
