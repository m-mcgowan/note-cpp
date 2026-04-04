#pragma once

/// @file escape_decoder.hpp
/// Utf8EscapeDecoder — decodes JSON string escape sequences.
///
/// After the lexer sees a backslash, it feeds subsequent characters
/// here. The decoder emits decoded bytes via a callback (1 byte for
/// simple escapes like \n, up to 3 bytes for \uXXXX UTF-8).

#include <cstdint>

namespace note {

struct Utf8EscapeDecoder {
    uint16_t unicode_acc = 0;
    uint8_t hex_remaining = 0;

    void reset() {
        unicode_acc = 0;
        hex_remaining = 0;
    }

    bool in_unicode() const { return hex_remaining > 0; }

    /// Feed the character immediately after backslash.
    /// Calls emit(char) for each decoded byte.
    /// Returns false on invalid escape character.
    template<typename EmitFn>
    bool feed(char c, EmitFn&& emit) {
        switch (c) {
        case '"':  emit('"');  return true;
        case '\\': emit('\\'); return true;
        case '/':  emit('/');  return true;
        case 'b':  emit('\b'); return true;
        case 'f':  emit('\f'); return true;
        case 'n':  emit('\n'); return true;
        case 'r':  emit('\r'); return true;
        case 't':  emit('\t'); return true;
        case 'u':
            unicode_acc = 0;
            hex_remaining = 4;
            return true;
        default:
            return false;
        }
    }

    /// Feed one hex digit for \uXXXX. Calls emit when all 4 digits
    /// are received, encoding the code point as UTF-8.
    /// Returns false on invalid hex digit.
    template<typename EmitFn>
    bool feed_hex(char c, EmitFn&& emit) {
        uint8_t nib;
        if (c >= '0' && c <= '9') nib = static_cast<uint8_t>(c - '0');
        else if (c >= 'a' && c <= 'f') nib = static_cast<uint8_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nib = static_cast<uint8_t>(c - 'A' + 10);
        else return false;

        unicode_acc = static_cast<uint16_t>((unicode_acc << 4) | nib);
        --hex_remaining;

        if (hex_remaining == 0) {
            emit_utf8(unicode_acc, emit);
        }
        return true;
    }

private:
    template<typename EmitFn>
    static void emit_utf8(uint16_t cp, EmitFn&& emit) {
        if (cp < 0x80) {
            emit(static_cast<char>(cp));
        } else if (cp < 0x800) {
            emit(static_cast<char>(0xC0 | (cp >> 6)));
            emit(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            // Surrogate pair — emit replacement character
            emit('?');
        } else {
            emit(static_cast<char>(0xE0 | (cp >> 12)));
            emit(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            emit(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
};

/// Minimal escape decoder — handles basic escapes only, no \uXXXX.
/// Notecard responses never use unicode escapes.
struct BasicEscapeDecoder {
    bool in_unicode() const { return false; }
    void reset() {}

    template<typename EmitFn>
    bool feed(char c, EmitFn&& emit) {
        switch (c) {
        case '"':  emit('"');  return true;
        case '\\': emit('\\'); return true;
        case '/':  emit('/');  return true;
        case 'b':  emit('\b'); return true;
        case 'f':  emit('\f'); return true;
        case 'n':  emit('\n'); return true;
        case 'r':  emit('\r'); return true;
        case 't':  emit('\t'); return true;
        default:   return false;
        }
    }

    template<typename EmitFn>
    bool feed_hex(char, EmitFn&&) { return false; }
};

} // namespace note
