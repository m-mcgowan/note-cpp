#pragma once

/// @file json_lexer.hpp
/// JsonLexer — zero-buffer push-based JSON tokenizer.
///
/// Consumes one byte at a time via feed(). Emits LexerEvent callbacks
/// for structural tokens, key/value characters, and parsed values.
/// Validates JSON grammar. No allocation, no buffers.
///
/// Template strategies (independently testable, zero overhead):
///   Stack         — nesting validation (BitStack<uint32_t>)
///   NumberParser  — incremental number parsing (IncrementalNumber)
///   EscapeDecoder — JSON string escape decoding (Utf8EscapeDecoder)
///
/// Usage:
///   DefaultLexer lexer;
///   for (uint8_t byte : input) {
///       lexer.feed(byte, [](LexerEvent ev) { ... });
///   }

#include <note/compiler.hpp>
#include <note/lexer/event.hpp>
#include <note/lexer/bit_stack.hpp>
#include <note/lexer/number_parser.hpp>
#include <note/lexer/escape_decoder.hpp>

#include <cstdint>

namespace note {

template<typename Stack = BitStack<uint32_t>,
         typename NumberParser = IncrementalNumber,
         typename EscapeDecoder = Utf8EscapeDecoder>
class JsonLexer {
public:
    /// Feed one byte. Calls handler(LexerEvent) for each event produced.
    /// Multiple events may fire from a single byte.
    template<typename Handler>
    void feed(uint8_t byte, Handler&& handler) {
        if (error_) return;
        char c = static_cast<char>(byte);

        switch (state_) {
        case State::TopLevel:     on_value_start(c, handler); break;
        case State::ObjectStart:  on_object_start(c, handler); break;
        case State::ObjectKey:    on_object_key(c, handler); break;
        case State::ObjectColon:  on_object_colon(c, handler); break;
        case State::ObjectValue:  on_value_start(c, handler); break;
        case State::ObjectNext:   on_object_next(c, handler); break;
        case State::ArrayStart:   on_array_start(c, handler); break;
        case State::ArrayValue:   on_value_start(c, handler); break;
        case State::ArrayNext:    on_array_next(c, handler); break;
        case State::InString:     on_in_string(c, handler); break;
        case State::InEscape:     on_in_escape(c, handler); break;
        case State::InUnicode:    on_in_unicode(c, handler); break;
        case State::InNumber:     on_in_number(c, handler); break;
        case State::InLiteral:    on_in_literal(c, handler); break;
        case State::Done:         break;
        }
    }

    bool has_error() const { return error_; }
    bool is_done() const { return state_ == State::Done; }

private:
    enum class State : uint8_t {
        TopLevel,       // expecting start of root value
        ObjectStart,    // after {, expecting key or }
        ObjectKey,      // expecting key string
        ObjectColon,    // after key, expecting :
        ObjectValue,    // after :, expecting value
        ObjectNext,     // after value, expecting , or }
        ArrayStart,     // after [, expecting value or ]
        ArrayValue,     // expecting array element value
        ArrayNext,      // after value, expecting , or ]
        InString,       // inside "..." (key or value)
        InEscape,       // after backslash in string
        InUnicode,      // in \uXXXX hex digits
        InNumber,       // in a number literal
        InLiteral,      // in true/false/null
        Done,           // root value complete
    };

    enum class StringContext : uint8_t {
        Key,
        Value,
    };

    // What state to return to after a value completes
    State container_next_state() const {
        if (stack_.empty()) return State::Done;
        if (stack_.in_object()) return State::ObjectNext;
        return State::ArrayNext;
    }

    // ── Whitespace ──────────────────────────────────────────────────────

    static bool is_ws(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    // ── State handlers ──────────────────────────────────────────────────

    template<typename H>
    void on_value_start(char c, H&& h) {
        if (is_ws(c)) return;
        if (c == '{') {
            if (!stack_.push_object()) return emit_error(NOTE_ERR("\1"), h);
            emit(LexerEvent::object_begin(), h);
            state_ = State::ObjectStart;
        } else if (c == '[') {
            if (!stack_.push_array()) return emit_error(NOTE_ERR("\1"), h);
            emit(LexerEvent::array_begin(), h);
            state_ = State::ArrayStart;
        } else if (c == '"') {
            string_ctx_ = StringContext::Value;
            state_ = State::InString;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            number_.reset();
            if (c == '-') { number_.set_negative(); number_state_ = NumState::IntStart; }
            else { number_.add_digit(static_cast<uint8_t>(c - '0')); number_state_ = (c == '0') ? NumState::Zero : NumState::Int; }
            state_ = State::InNumber;
        } else if (c == 't') {
            literal_ = "true";
            literal_pos_ = 1;
            literal_len_ = 4;
            state_ = State::InLiteral;
        } else if (c == 'f') {
            literal_ = "false";
            literal_pos_ = 1;
            literal_len_ = 5;
            state_ = State::InLiteral;
        } else if (c == 'n') {
            literal_ = "null";
            literal_pos_ = 1;
            literal_len_ = 4;
            state_ = State::InLiteral;
        } else {
            emit_error(NOTE_ERR("\1"), h);
        }
    }

    template<typename H>
    void on_object_start(char c, H&& h) {
        if (is_ws(c)) return;
        if (c == '}') {
            if (!stack_.pop_object()) return emit_error(NOTE_ERR("\1"), h);
            emit(LexerEvent::object_end(), h);
            state_ = container_next_state();
        } else if (c == '"') {
            string_ctx_ = StringContext::Key;
            state_ = State::InString;
        } else {
            emit_error(NOTE_ERR("\1"), h);
        }
    }

    template<typename H>
    void on_object_key(char c, H&& h) {
        // After a comma in an object — must be a key
        if (is_ws(c)) return;
        if (c == '"') {
            string_ctx_ = StringContext::Key;
            state_ = State::InString;
        } else {
            emit_error(NOTE_ERR("\1"), h);
        }
    }

    template<typename H>
    void on_object_colon(char c, H&& h) {
        if (is_ws(c)) return;
        if (c == ':') {
            state_ = State::ObjectValue;
        } else {
            emit_error(NOTE_ERR("\1"), h);
        }
    }

    template<typename H>
    void on_object_next(char c, H&& h) {
        if (is_ws(c)) return;
        if (c == ',') {
            state_ = State::ObjectKey;
        } else if (c == '}') {
            if (!stack_.pop_object()) return emit_error(NOTE_ERR("\1"), h);
            emit(LexerEvent::object_end(), h);
            state_ = container_next_state();
        } else {
            emit_error(NOTE_ERR("\1"), h);
        }
    }

    template<typename H>
    void on_array_start(char c, H&& h) {
        if (is_ws(c)) return;
        if (c == ']') {
            if (!stack_.pop_array()) return emit_error(NOTE_ERR("\1"), h);
            emit(LexerEvent::array_end(), h);
            state_ = container_next_state();
        } else {
            // First element — process as a value
            state_ = State::ArrayValue;
            on_value_start(c, h);
        }
    }

    template<typename H>
    void on_array_next(char c, H&& h) {
        if (is_ws(c)) return;
        if (c == ',') {
            state_ = State::ArrayValue;
        } else if (c == ']') {
            if (!stack_.pop_array()) return emit_error(NOTE_ERR("\1"), h);
            emit(LexerEvent::array_end(), h);
            state_ = container_next_state();
        } else {
            emit_error(NOTE_ERR("\1"), h);
        }
    }

    // ── Strings ──────────────────────────────────────────────────────────

    template<typename H>
    void on_in_string(char c, H&& h) {
        if (c == '\\') {
            state_ = State::InEscape;
        } else if (c == '"') {
            // End of string
            if (string_ctx_ == StringContext::Key) {
                emit(LexerEvent::key_end(), h);
                state_ = State::ObjectColon;
            } else {
                emit(LexerEvent::string_end(), h);
                state_ = container_next_state();
            }
        } else if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
            emit_error(NOTE_ERR("\1"), h);
        } else {
            if (string_ctx_ == StringContext::Key)
                emit(LexerEvent::key_char(c), h);
            else
                emit(LexerEvent::string_char(c), h);
        }
    }

    template<typename H>
    void on_in_escape(char c, H&& h) {
        auto emit_char = [&](char decoded) {
            if (string_ctx_ == StringContext::Key)
                emit(LexerEvent::key_char(decoded), h);
            else
                emit(LexerEvent::string_char(decoded), h);
        };
        if (!escape_.feed(c, emit_char)) {
            emit_error(NOTE_ERR("\1"), h);
            return;
        }
        if (escape_.in_unicode())
            state_ = State::InUnicode;
        else
            state_ = State::InString;
    }

    template<typename H>
    void on_in_unicode(char c, H&& h) {
        auto emit_char = [&](char decoded) {
            if (string_ctx_ == StringContext::Key)
                emit(LexerEvent::key_char(decoded), h);
            else
                emit(LexerEvent::string_char(decoded), h);
        };
        if (!escape_.feed_hex(c, emit_char)) {
            emit_error(NOTE_ERR("\1"), h);
            return;
        }
        if (!escape_.in_unicode())
            state_ = State::InString;
    }

    // ── Numbers ──────────────────────────────────────────────────────────

    enum class NumState : uint8_t {
        IntStart,   // after minus, need digit
        Zero,       // leading zero — only . or e allowed next
        Int,        // in integer digits
        FracStart,  // after dot, need digit
        Frac,       // in fractional digits
        ExpSign,    // after e/E, optional +/-
        ExpStart,   // after exp sign, need digit
        Exp,        // in exponent digits
    };

    template<typename H>
    void on_in_number(char c, H&& h) {
        switch (number_state_) {
        case NumState::IntStart:
            if (c >= '0' && c <= '9') {
                number_.add_digit(static_cast<uint8_t>(c - '0'));
                number_state_ = (c == '0') ? NumState::Zero : NumState::Int;
            } else {
                emit_error(NOTE_ERR("\1"), h);
            }
            return;
        case NumState::Zero:
            if (c == '.') { number_.start_fraction(); number_state_ = NumState::FracStart; return; }
            if (c == 'e' || c == 'E') { number_.start_exponent(); number_state_ = NumState::ExpSign; return; }
            // Any other char terminates the number
            break;
        case NumState::Int:
            if (c >= '0' && c <= '9') { number_.add_digit(static_cast<uint8_t>(c - '0')); return; }
            if (c == '.') { number_.start_fraction(); number_state_ = NumState::FracStart; return; }
            if (c == 'e' || c == 'E') { number_.start_exponent(); number_state_ = NumState::ExpSign; return; }
            break;
        case NumState::FracStart:
            if (c >= '0' && c <= '9') { number_.add_frac_digit(static_cast<uint8_t>(c - '0')); number_state_ = NumState::Frac; return; }
            emit_error(NOTE_ERR("\1"), h);
            return;
        case NumState::Frac:
            if (c >= '0' && c <= '9') { number_.add_frac_digit(static_cast<uint8_t>(c - '0')); return; }
            if (c == 'e' || c == 'E') { number_.start_exponent(); number_state_ = NumState::ExpSign; return; }
            break;
        case NumState::ExpSign:
            if (c == '+') { number_state_ = NumState::ExpStart; return; }
            if (c == '-') { number_.set_exp_negative(); number_state_ = NumState::ExpStart; return; }
            if (c >= '0' && c <= '9') { number_.add_exp_digit(static_cast<uint8_t>(c - '0')); number_state_ = NumState::Exp; return; }
            emit_error(NOTE_ERR("\1"), h);
            return;
        case NumState::ExpStart:
            if (c >= '0' && c <= '9') { number_.add_exp_digit(static_cast<uint8_t>(c - '0')); number_state_ = NumState::Exp; return; }
            emit_error(NOTE_ERR("\1"), h);
            return;
        case NumState::Exp:
            if (c >= '0' && c <= '9') { number_.add_exp_digit(static_cast<uint8_t>(c - '0')); return; }
            break;
        }

        // Number terminated by this character — emit the number, then
        // re-process the character in the parent state.
        if (number_.is_integer())
            emit(LexerEvent::make_int(number_.to_int32()), h);
        else
            emit(LexerEvent::make_float(number_.to_float()), h);

        state_ = container_next_state();
        // Re-feed the terminating character
        feed(static_cast<uint8_t>(c), h);
    }

    // ── Literals (true, false, null) ─────────────────────────────────────

    template<typename H>
    void on_in_literal(char c, H&& h) {
        if (c != literal_[literal_pos_]) {
            emit_error(NOTE_ERR("\1"), h);
            return;
        }
        ++literal_pos_;
        if (literal_pos_ >= literal_len_) {
            // Complete literal
            if (literal_[0] == 't') emit(LexerEvent::make_bool(true), h);
            else if (literal_[0] == 'f') emit(LexerEvent::make_bool(false), h);
            else emit(LexerEvent::null(), h);
            state_ = container_next_state();
        }
    }

    // ── Emit helpers ─────────────────────────────────────────────────────

    template<typename H>
    void emit(LexerEvent ev, H&& h) { h(ev); }

    template<typename H>
    void emit_error(const char* msg, H&& h) {
        error_ = true;
        h(LexerEvent::make_error(msg));
    }

    // ── State ────────────────────────────────────────────────────────────

    State state_ = State::TopLevel;
    StringContext string_ctx_ = StringContext::Value;
    NumState number_state_ = NumState::IntStart;
    bool error_ = false;

    Stack stack_{};
    NumberParser number_{};
    EscapeDecoder escape_{};

    const char* literal_ = nullptr;
    uint8_t literal_pos_ = 0;
    uint8_t literal_len_ = 0;
};

using DefaultLexer = JsonLexer<>;

} // namespace note
