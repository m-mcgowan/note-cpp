// Minimal parsers for JSON number strings. Shared by json_sax (for its
// on_number callback helpers) and json_scan (for scan::get<T> without
// pulling in the full SAX parser).
//
// Both are constexpr so they can be used in compile-time contexts, but
// they're also tiny enough to be cheap at runtime on AVR.
#pragma once

#include <note/types.hpp>

namespace note {

constexpr json_int_t parse_int(string_view raw, json_int_t def = 0) {
    if (raw.empty()) return def;
    json_int_t result = 0;
    bool negative = false;
    size_t i = 0;
    if (raw[0] == '-') { negative = true; ++i; }
    for (; i < raw.size(); ++i) {
        char c = raw[i];
        if (c < '0' || c > '9') {
            if (c == '.') break;  // truncate decimal
            return def;
        }
        result = result * 10 + (c - '0');
    }
    return negative ? -result : result;
}

constexpr double parse_double(string_view raw, double def = 0.0) {
    if (raw.empty()) return def;
    double result = 0.0;
    bool negative = false;
    size_t i = 0;
    if (raw[0] == '-') { negative = true; ++i; }
    for (; i < raw.size() && raw[i] != '.' && raw[i] != 'e' && raw[i] != 'E'; ++i) {
        if (raw[i] < '0' || raw[i] > '9') return def;
        result = result * 10.0 + (raw[i] - '0');
    }
    if (i < raw.size() && raw[i] == '.') {
        ++i;
        double frac = 0.1;
        for (; i < raw.size() && raw[i] != 'e' && raw[i] != 'E'; ++i) {
            if (raw[i] < '0' || raw[i] > '9') return def;
            result += (raw[i] - '0') * frac;
            frac *= 0.1;
        }
    }
    if (i < raw.size() && (raw[i] == 'e' || raw[i] == 'E')) {
        ++i;
        bool neg_exp = false;
        if (i < raw.size() && (raw[i] == '+' || raw[i] == '-')) {
            neg_exp = (raw[i] == '-');
            ++i;
        }
        int exp = 0;
        for (; i < raw.size(); ++i) {
            if (raw[i] < '0' || raw[i] > '9') return def;
            exp = exp * 10 + (raw[i] - '0');
        }
        double factor = 1.0;
        for (int e = 0; e < exp; ++e) factor *= 10.0;
        result = neg_exp ? (result / factor) : (result * factor);
    }
    return negative ? -result : result;
}

} // namespace note
