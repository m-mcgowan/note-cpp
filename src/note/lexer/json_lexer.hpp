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
/// The Handler callback is type-erased to a function pointer so that
/// all internal state handlers are non-template regular methods.
/// This prevents the compiler from inlining 12 handlers into feed(),
/// reducing flash on constrained targets.
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
    /// Type-erased event callback: handler(ctx, event).
    using EventFn = void(*)(void*, const LexerEvent&);

    /// Feed one byte. Calls handler(LexerEvent) for each event produced.
    /// The handler is type-erased internally so all state handlers are
    /// non-template — the compiler can outline them for smaller flash.
    template<typename Handler>
    void feed(uint8_t byte, Handler&& handler) {
        // Type-erase: store handler as function pointer + context.
        // The trampoline lambda is stateless → compiles to a plain function.
        EventFn fn = [](void* ctx, const LexerEvent& ev) {
            (*static_cast<std::remove_reference_t<Handler>*>(ctx))(ev);
        };
        feed_impl(byte, fn, &handler);
    }

    bool has_error() const { return error_; }
    bool is_done() const { return state_ == State::Done; }

private:
    // ── Core state machine (all non-template) ───────────────────────────

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

    enum class StringContext : uint8_t { Key, Value };

    /// Main dispatch — routes byte to the current state handler.
    void feed_impl(uint8_t byte, EventFn fn, void* ctx) {
        if (error_) return;
        char c = static_cast<char>(byte);

        switch (state_) {
        case State::TopLevel:     on_value_start(c, fn, ctx); break;
        case State::ObjectStart:  on_object_start(c, fn, ctx); break;
        case State::ObjectKey:    on_object_key(c, fn, ctx); break;
        case State::ObjectColon:  on_object_colon(c, fn, ctx); break;
        case State::ObjectValue:  on_value_start(c, fn, ctx); break;
        case State::ObjectNext:   on_object_next(c, fn, ctx); break;
        case State::ArrayStart:   on_array_start(c, fn, ctx); break;
        case State::ArrayValue:   on_value_start(c, fn, ctx); break;
        case State::ArrayNext:    on_array_next(c, fn, ctx); break;
        case State::InString:     on_in_string(c, fn, ctx); break;
        case State::InEscape:     on_in_escape(c, fn, ctx); break;
        case State::InUnicode:    on_in_unicode(c, fn, ctx); break;
        case State::InNumber:     on_in_number(c, fn, ctx); break;
        case State::InLiteral:    on_in_literal(c, fn, ctx); break;
        case State::Done:         break;
        }
    }

    // ── Helpers ─────────────────────────────────────────────────────────

    /// What state to return to after a value completes.
    State container_next_state() const {
        if (stack_.empty()) return State::Done;
        if (stack_.in_object()) return State::ObjectNext;
        return State::ArrayNext;
    }

    static bool is_ws(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    void emit(const LexerEvent& ev, EventFn fn, void* ctx) { fn(ctx, ev); }

    void emit_error(const char* msg, EventFn fn, void* ctx) {
        error_ = true;
        fn(ctx, LexerEvent::make_error(msg));
    }

    // ── State handlers (all non-template) ───────────────────────────────

    /// Expecting start of a JSON value: object, array, string, number,
    /// or literal (true/false/null). Called from TopLevel, ObjectValue,
    /// and ArrayValue states.
    void on_value_start(char c, EventFn fn, void* ctx) {
        if (is_ws(c)) return;
        if (c == '{') {
            if (!stack_.push_object()) return emit_error(NOTE_ERR("\1"), fn, ctx);
            emit(LexerEvent::object_begin(), fn, ctx);
            state_ = State::ObjectStart;
        } else if (c == '[') {
            if (!stack_.push_array()) return emit_error(NOTE_ERR("\1"), fn, ctx);
            emit(LexerEvent::array_begin(), fn, ctx);
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
            emit_error(NOTE_ERR("\1"), fn, ctx);
        }
    }

    /// After '{' — expecting first key string or closing '}'.
    void on_object_start(char c, EventFn fn, void* ctx) {
        if (is_ws(c)) return;
        if (c == '}') {
            if (!stack_.pop_object()) return emit_error(NOTE_ERR("\1"), fn, ctx);
            emit(LexerEvent::object_end(), fn, ctx);
            state_ = container_next_state();
        } else if (c == '"') {
            string_ctx_ = StringContext::Key;
            state_ = State::InString;
        } else {
            emit_error(NOTE_ERR("\1"), fn, ctx);
        }
    }

    /// After comma in object — expecting next key string.
    void on_object_key(char c, EventFn fn, void* ctx) {
        if (is_ws(c)) return;
        if (c == '"') {
            string_ctx_ = StringContext::Key;
            state_ = State::InString;
        } else {
            emit_error(NOTE_ERR("\1"), fn, ctx);
        }
    }

    /// After key string — expecting ':'.
    void on_object_colon(char c, EventFn fn, void* ctx) {
        if (is_ws(c)) return;
        if (c == ':') {
            state_ = State::ObjectValue;
        } else {
            emit_error(NOTE_ERR("\1"), fn, ctx);
        }
    }

    /// After object value — expecting ',' or '}'.
    void on_object_next(char c, EventFn fn, void* ctx) {
        if (is_ws(c)) return;
        if (c == ',') {
            state_ = State::ObjectKey;
        } else if (c == '}') {
            if (!stack_.pop_object()) return emit_error(NOTE_ERR("\1"), fn, ctx);
            emit(LexerEvent::object_end(), fn, ctx);
            state_ = container_next_state();
        } else {
            emit_error(NOTE_ERR("\1"), fn, ctx);
        }
    }

    /// After '[' — expecting first element or closing ']'.
    void on_array_start(char c, EventFn fn, void* ctx) {
        if (is_ws(c)) return;
        if (c == ']') {
            if (!stack_.pop_array()) return emit_error(NOTE_ERR("\1"), fn, ctx);
            emit(LexerEvent::array_end(), fn, ctx);
            state_ = container_next_state();
        } else {
            state_ = State::ArrayValue;
            on_value_start(c, fn, ctx);
        }
    }

    /// After array element — expecting ',' or ']'.
    void on_array_next(char c, EventFn fn, void* ctx) {
        if (is_ws(c)) return;
        if (c == ',') {
            state_ = State::ArrayValue;
        } else if (c == ']') {
            if (!stack_.pop_array()) return emit_error(NOTE_ERR("\1"), fn, ctx);
            emit(LexerEvent::array_end(), fn, ctx);
            state_ = container_next_state();
        } else {
            emit_error(NOTE_ERR("\1"), fn, ctx);
        }
    }

    // ── String handling ─────────────────────────────────────────────────

    /// Inside a quoted string — accumulating characters.
    void on_in_string(char c, EventFn fn, void* ctx) {
        if (c == '\\') {
            state_ = State::InEscape;
        } else if (c == '"') {
            if (string_ctx_ == StringContext::Key) {
                emit(LexerEvent::key_end(), fn, ctx);
                state_ = State::ObjectColon;
            } else {
                emit(LexerEvent::string_end(), fn, ctx);
                state_ = container_next_state();
            }
        } else if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
            emit_error(NOTE_ERR("\1"), fn, ctx);
        } else {
            if (string_ctx_ == StringContext::Key)
                emit(LexerEvent::key_char(c), fn, ctx);
            else
                emit(LexerEvent::string_char(c), fn, ctx);
        }
    }

    /// After backslash — decode escape sequence.
    void on_in_escape(char c, EventFn fn, void* ctx) {
        auto emit_char = [&](char decoded) {
            if (string_ctx_ == StringContext::Key)
                emit(LexerEvent::key_char(decoded), fn, ctx);
            else
                emit(LexerEvent::string_char(decoded), fn, ctx);
        };
        if (!escape_.feed(c, emit_char)) {
            emit_error(NOTE_ERR("\1"), fn, ctx);
            return;
        }
        if (escape_.in_unicode())
            state_ = State::InUnicode;
        else
            state_ = State::InString;
    }

    /// In \uXXXX hex digits.
    void on_in_unicode(char c, EventFn fn, void* ctx) {
        auto emit_char = [&](char decoded) {
            if (string_ctx_ == StringContext::Key)
                emit(LexerEvent::key_char(decoded), fn, ctx);
            else
                emit(LexerEvent::string_char(decoded), fn, ctx);
        };
        if (!escape_.feed_hex(c, emit_char)) {
            emit_error(NOTE_ERR("\1"), fn, ctx);
            return;
        }
        if (!escape_.in_unicode())
            state_ = State::InString;
    }

    // ── Number handling ─────────────────────────────────────────────────

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

    /// Incremental number parsing — one digit/char at a time.
    void on_in_number(char c, EventFn fn, void* ctx) {
        switch (number_state_) {
        case NumState::IntStart:
            if (c >= '0' && c <= '9') {
                number_.add_digit(static_cast<uint8_t>(c - '0'));
                number_state_ = (c == '0') ? NumState::Zero : NumState::Int;
            } else {
                emit_error(NOTE_ERR("\1"), fn, ctx);
            }
            return;
        case NumState::Zero:
            if (c == '.') { number_.start_fraction(); number_state_ = NumState::FracStart; return; }
            if (c == 'e' || c == 'E') { number_.start_exponent(); number_state_ = NumState::ExpSign; return; }
            break;
        case NumState::Int:
            if (c >= '0' && c <= '9') { number_.add_digit(static_cast<uint8_t>(c - '0')); return; }
            if (c == '.') { number_.start_fraction(); number_state_ = NumState::FracStart; return; }
            if (c == 'e' || c == 'E') { number_.start_exponent(); number_state_ = NumState::ExpSign; return; }
            break;
        case NumState::FracStart:
            if (c >= '0' && c <= '9') { number_.add_frac_digit(static_cast<uint8_t>(c - '0')); number_state_ = NumState::Frac; return; }
            emit_error(NOTE_ERR("\1"), fn, ctx);
            return;
        case NumState::Frac:
            if (c >= '0' && c <= '9') { number_.add_frac_digit(static_cast<uint8_t>(c - '0')); return; }
            if (c == 'e' || c == 'E') { number_.start_exponent(); number_state_ = NumState::ExpSign; return; }
            break;
        case NumState::ExpSign:
            if (c == '+') { number_state_ = NumState::ExpStart; return; }
            if (c == '-') { number_.set_exp_negative(); number_state_ = NumState::ExpStart; return; }
            if (c >= '0' && c <= '9') { number_.add_exp_digit(static_cast<uint8_t>(c - '0')); number_state_ = NumState::Exp; return; }
            emit_error(NOTE_ERR("\1"), fn, ctx);
            return;
        case NumState::ExpStart:
            if (c >= '0' && c <= '9') { number_.add_exp_digit(static_cast<uint8_t>(c - '0')); number_state_ = NumState::Exp; return; }
            emit_error(NOTE_ERR("\1"), fn, ctx);
            return;
        case NumState::Exp:
            if (c >= '0' && c <= '9') { number_.add_exp_digit(static_cast<uint8_t>(c - '0')); return; }
            break;
        }

        // Number terminated by this character — emit, then re-process.
        if (number_.is_integer())
            emit(LexerEvent::make_int(number_.to_int32()), fn, ctx);
        else
            emit(LexerEvent::make_float(number_.to_float()), fn, ctx);

        state_ = container_next_state();
        feed_impl(static_cast<uint8_t>(c), fn, ctx);
    }

    // ── Literal handling (true, false, null) ────────────────────────────

    /// Character-by-character literal matching.
    void on_in_literal(char c, EventFn fn, void* ctx) {
        if (c != literal_[literal_pos_]) {
            emit_error(NOTE_ERR("\1"), fn, ctx);
            return;
        }
        ++literal_pos_;
        if (literal_pos_ >= literal_len_) {
            if (literal_[0] == 't') emit(LexerEvent::make_bool(true), fn, ctx);
            else if (literal_[0] == 'f') emit(LexerEvent::make_bool(false), fn, ctx);
            else emit(LexerEvent::null(), fn, ctx);
            state_ = container_next_state();
        }
    }

    // ── Member state ────────────────────────────────────────────────────

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

#if defined(NOTE_MINIMAL) && !defined(NOTE_UNICODE_ESCAPES)
using DefaultLexer = JsonLexer<BitStack<uint8_t>, CompactNumber, BasicEscapeDecoder>;
#else
using DefaultLexer = JsonLexer<>;
#endif

} // namespace note
