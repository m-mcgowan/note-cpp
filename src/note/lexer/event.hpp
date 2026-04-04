#pragma once

/// @file event.hpp
/// LexerEvent — token emitted by the JSON lexer.

#include <cstdint>

namespace note {

struct LexerEvent {
    enum Tag : uint8_t {
        ObjectBegin,    // {
        ObjectEnd,      // }
        ArrayBegin,     // [
        ArrayEnd,       // ]
        KeyChar,        // one decoded key character
        KeyEnd,         // end of key string
        StringChar,     // one decoded value string character
        StringEnd,      // end of value string
        Integer,        // complete integer value
        Float,          // complete floating-point value
        Bool,           // true or false
        Null,           // null
        Error,          // parse error
    } tag;

    union {
        char ch;            // KeyChar, StringChar
        int32_t integer;    // Integer
        double floating;    // Float
        bool boolean;       // Bool
        const char* error;  // Error — static string
    };

    static LexerEvent object_begin()          { return {ObjectBegin, {}}; }
    static LexerEvent object_end()            { return {ObjectEnd, {}}; }
    static LexerEvent array_begin()           { return {ArrayBegin, {}}; }
    static LexerEvent array_end()             { return {ArrayEnd, {}}; }
    static LexerEvent key_char(char c)        { LexerEvent e{KeyChar, {}}; e.ch = c; return e; }
    static LexerEvent key_end()               { return {KeyEnd, {}}; }
    static LexerEvent string_char(char c)     { LexerEvent e{StringChar, {}}; e.ch = c; return e; }
    static LexerEvent string_end()            { return {StringEnd, {}}; }
    static LexerEvent make_int(int32_t v)     { LexerEvent e{Integer, {}}; e.integer = v; return e; }
    static LexerEvent make_float(double v)    { LexerEvent e{Float, {}}; e.floating = v; return e; }
    static LexerEvent make_bool(bool v)       { LexerEvent e{Bool, {}}; e.boolean = v; return e; }
    static LexerEvent null()                  { return {Null, {}}; }
    static LexerEvent make_error(const char* msg) { LexerEvent e{Error, {}}; e.error = msg; return e; }
};

} // namespace note
