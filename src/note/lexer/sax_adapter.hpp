#pragma once

/// @file sax_adapter.hpp
/// SaxAdapter — bridges JsonLexer events to the JsonSink interface.
///
/// Accumulates KeyChar events into a key buffer and StringChar events
/// into a value buffer, then delivers complete (key, value) pairs to
/// a JsonSink. Buffer sizes are developer-controlled.
///
/// The adapter maintains a key stack so that nested objects and arrays
/// deliver the correct key context to the sink.
///
/// Usage:
///   char buf[384];
///   SaxStreamBuf sbuf(buf);  // partitions into key + value regions
///   SaxAdapter adapter(sbuf, sink);
///   // Feed LexerEvents from the lexer...
///   adapter.on_event(ev);

#include <note/json_sax.hpp>
#include <note/json_sax_streaming.hpp>  // SaxStreamBuf
#include <note/lexer/event.hpp>
#include <note/types.hpp>

#include <cstdint>

namespace note {

template<typename SinkT = JsonSink>
class SaxAdapter {
public:
    SaxAdapter(SaxStreamBuf& buf, SinkT& sink)
        : sink_(sink)
        , key_buf_(buf.key)
        , key_cap_(buf.key_size)
        , val_buf_(buf.val)
        , val_cap_(buf.val_size)
    {}

    /// Process one lexer event. Call this from the lexer callback.
    void on_event(const LexerEvent& ev) {
        switch (ev.tag) {
        case LexerEvent::ObjectBegin:
            sink_.on_object_begin(current_key());
            push_key();
            break;
        case LexerEvent::ObjectEnd:
            pop_key();
            sink_.on_object_end(current_key());
            break;
        case LexerEvent::ArrayBegin:
            sink_.on_array_begin(current_key());
            // Array elements inherit the current key — don't push/pop
            in_array_depth_++;
            break;
        case LexerEvent::ArrayEnd:
            if (in_array_depth_ > 0) in_array_depth_--;
            sink_.on_array_end(current_key());
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
            sink_.on_string(current_key(), string_view(val_buf_, val_len_));
            val_len_ = 0;
            break;
        case LexerEvent::Integer:
            sink_.on_int(current_key(), ev.integer);
            break;
        case LexerEvent::Float:
            sink_.on_float(current_key(), ev.floating);
            break;
        case LexerEvent::Bool:
            sink_.on_bool(current_key(), ev.boolean);
            break;
        case LexerEvent::Null:
            sink_.on_null(current_key());
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
        sink_.reset();
    }

private:
    string_view current_key() const {
        return string_view(key_buf_, key_len_);
    }

    /// Save the current key onto the stack (entering a nested object).
    /// The key buffer is partitioned: stack grows from the end, current
    /// key lives at the start.
    void push_key() {
        if (stack_depth_ < kMaxDepth) {
            uint8_t save_len = key_len_ < kMaxKeyPerLevel ? static_cast<uint8_t>(key_len_) : kMaxKeyPerLevel;
            for (uint8_t i = 0; i < save_len; ++i)
                key_stack_[stack_depth_].saved[i] = key_buf_[i];
            key_stack_[stack_depth_].length = save_len;
            ++stack_depth_;
        }
        key_len_ = 0;  // reset for the nested object's keys
    }

    /// Restore the parent key (leaving a nested object).
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

    SinkT& sink_;

    // Key buffer (developer-provided via SaxStreamBuf)
    char* key_buf_;
    size_t key_cap_;
    size_t key_len_ = 0;

    // Value buffer (developer-provided via SaxStreamBuf)
    char* val_buf_;
    size_t val_cap_;
    size_t val_len_ = 0;

    // Key stack for nesting. Each level saves the parent key.
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
