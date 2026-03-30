#pragma once
/// @file json_validate.hpp
/// Consteval JSON validation — compile-time check that a string literal
/// is well-formed JSON and a top-level object.
///
/// Usage (C++20):
///   static_assert(note::json_valid(R"({"temp":22.5})"));
///   static_assert(!note::json_valid("[1,2,3]"));       // not an object
///   static_assert(!note::json_valid(R"({"temp":})"));   // malformed

#include <string_view>
#include <cstddef>

namespace note {

namespace detail {

/// Minimal constexpr JSON validator.
/// Returns true if the string is well-formed JSON and the top-level value
/// is an object. Does not build a tree — just validates structure.
class JsonValidator {
    std::string_view src_;
    std::size_t pos_ = 0;

    constexpr char peek() const {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }

    constexpr char next() {
        return pos_ < src_.size() ? src_[pos_++] : '\0';
    }

    constexpr void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    constexpr bool parse_string() {
        if (next() != '"') return false;
        while (pos_ < src_.size()) {
            char c = next();
            if (c == '"') return true;
            if (c == '\\') {
                char e = next();
                if (e == 'u') {
                    // \uXXXX — consume 4 hex digits
                    for (int i = 0; i < 4; ++i) {
                        char h = next();
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')))
                            return false;
                    }
                } else if (e != '"' && e != '\\' && e != '/' && e != 'b' &&
                           e != 'f' && e != 'n' && e != 'r' && e != 't') {
                    return false;  // invalid escape
                }
            }
        }
        return false;  // unterminated string
    }

    constexpr bool parse_number() {
        if (peek() == '-') next();
        if (peek() == '0') {
            next();
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') next();
        } else {
            return false;
        }
        if (peek() == '.') {
            next();
            if (peek() < '0' || peek() > '9') return false;
            while (peek() >= '0' && peek() <= '9') next();
        }
        if (peek() == 'e' || peek() == 'E') {
            next();
            if (peek() == '+' || peek() == '-') next();
            if (peek() < '0' || peek() > '9') return false;
            while (peek() >= '0' && peek() <= '9') next();
        }
        return true;
    }

    constexpr bool parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"') return parse_string();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't') {  // true
            return next() == 't' && next() == 'r' && next() == 'u' && next() == 'e';
        }
        if (c == 'f') {  // false
            return next() == 'f' && next() == 'a' && next() == 'l' && next() == 's' && next() == 'e';
        }
        if (c == 'n') {  // null
            return next() == 'n' && next() == 'u' && next() == 'l' && next() == 'l';
        }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        return false;
    }

    constexpr bool parse_object() {
        if (next() != '{') return false;
        skip_ws();
        if (peek() == '}') { next(); return true; }  // empty object
        while (true) {
            skip_ws();
            if (!parse_string()) return false;  // key
            skip_ws();
            if (next() != ':') return false;
            if (!parse_value()) return false;   // value
            skip_ws();
            char c = next();
            if (c == '}') return true;
            if (c != ',') return false;
        }
    }

    constexpr bool parse_array() {
        if (next() != '[') return false;
        skip_ws();
        if (peek() == ']') { next(); return true; }  // empty array
        while (true) {
            if (!parse_value()) return false;
            skip_ws();
            char c = next();
            if (c == ']') return true;
            if (c != ',') return false;
        }
    }

public:
    /// Validate that src is well-formed JSON with a top-level object.
    constexpr bool validate_object(std::string_view src) {
        src_ = src;
        pos_ = 0;
        skip_ws();
        if (peek() != '{') return false;  // must be an object
        if (!parse_object()) return false;
        skip_ws();
        return pos_ == src_.size();  // no trailing content
    }

    /// Validate that src is well-formed JSON (any type).
    constexpr bool validate(std::string_view src) {
        src_ = src;
        pos_ = 0;
        if (!parse_value()) return false;
        skip_ws();
        return pos_ == src_.size();
    }
};

} // namespace detail

#if __cplusplus >= 202002L

/// Consteval: true if the string is well-formed JSON and a top-level object.
consteval bool json_valid(std::string_view s) {
    detail::JsonValidator v;
    return v.validate_object(s);
}

/// Consteval: true if the string is well-formed JSON (any top-level type).
consteval bool json_valid_any(std::string_view s) {
    detail::JsonValidator v;
    return v.validate(s);
}

#else

/// Constexpr: true if the string is well-formed JSON and a top-level object.
constexpr bool json_valid(std::string_view s) {
    detail::JsonValidator v;
    return v.validate_object(s);
}

/// Constexpr: true if the string is well-formed JSON (any top-level type).
constexpr bool json_valid_any(std::string_view s) {
    detail::JsonValidator v;
    return v.validate(s);
}

#endif

} // namespace note
