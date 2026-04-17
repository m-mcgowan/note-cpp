#pragma once

/// @file number_parser.hpp
/// IncrementalNumber — builds JSON numbers from a digit stream.
///
/// No buffer. Accumulates value incrementally as digits arrive.
/// Tracks whether the number is integer or float (saw '.' or 'e').
/// Call to_integer() or to_float() after all digits are fed.

#include <note/types.hpp>

#include <cstdint>

namespace note {

struct IncrementalNumber {
    int64_t integer_acc = 0;
    double frac_acc = 0.0;
    double frac_divisor = 1.0;
    int32_t exp_acc = 0;
    int8_t sign = 1;
    int8_t exp_sign = 1;
    bool is_float = false;
    bool has_digits = false;

    void reset() {
        integer_acc = 0;
        frac_acc = 0.0;
        frac_divisor = 1.0;
        exp_acc = 0;
        sign = 1;
        exp_sign = 1;
        is_float = false;
        has_digits = false;
    }

    void set_negative() { sign = -1; }

    void add_digit(uint8_t d) {
        integer_acc = integer_acc * 10 + d;
        has_digits = true;
    }

    void start_fraction() {
        is_float = true;
        frac_divisor = 1.0;
        frac_acc = 0.0;
    }

    void add_frac_digit(uint8_t d) {
        frac_divisor *= 10.0;
        frac_acc += static_cast<double>(d) / frac_divisor;
        has_digits = true;
    }

    void start_exponent() {
        is_float = true;
        exp_acc = 0;
        exp_sign = 1;
    }

    void set_exp_negative() { exp_sign = -1; }

    void add_exp_digit(uint8_t d) {
        exp_acc = exp_acc * 10 + d;
    }

    bool is_integer() const { return !is_float; }

    json_int_t to_int() const {
        return static_cast<json_int_t>(sign * integer_acc);
    }

    double to_float() const {
        double v = static_cast<double>(integer_acc) + frac_acc;
        if (exp_acc != 0) {
            double exp = 1.0;
            for (int32_t i = 0; i < exp_acc; ++i) exp *= 10.0;
            if (exp_sign < 0) v /= exp; else v *= exp;
        }
        return sign * v;
    }
};

/// Compact number parser for constrained platforms.
/// Uses json_int_t (int32_t under NOTE_INT32_MATH, int64_t otherwise).
/// Notecard responses fit within int32_t range.
struct CompactNumber {
    json_int_t integer_acc = 0;
    int32_t frac_digits = 0;   // fractional digits as integer (e.g. 125 for .125)
    int8_t frac_count = 0;     // number of fractional digits
    int8_t exp_acc = 0;
    int8_t sign = 1;
    int8_t exp_sign = 1;
    bool is_float = false;

    void reset() {
        integer_acc = 0;
        frac_digits = 0;
        frac_count = 0;
        exp_acc = 0;
        sign = 1;
        exp_sign = 1;
        is_float = false;
    }

    void set_negative() { sign = -1; }

    void add_digit(uint8_t d) {
        integer_acc = integer_acc * 10 + d;
    }

    void start_fraction() { is_float = true; }

    void add_frac_digit(uint8_t d) {
        if (frac_count < 6) {  // 6 decimal digits of precision
            frac_digits = frac_digits * 10 + d;
            ++frac_count;
        }
    }

    void start_exponent() {
        is_float = true;
        exp_acc = 0;
        exp_sign = 1;
    }

    void set_exp_negative() { exp_sign = -1; }

    void add_exp_digit(uint8_t d) {
        exp_acc = static_cast<int8_t>(exp_acc * 10 + d);
    }

    bool is_integer() const { return !is_float; }

    json_int_t to_int() const {
        return sign * integer_acc;
    }

    double to_float() const {
        double v = static_cast<double>(integer_acc);
        if (frac_count > 0) {
            // Single division: frac_digits / 10^frac_count
            double divisor = 1.0;
            for (int8_t i = 0; i < frac_count; ++i) divisor *= 10.0;
            v += static_cast<double>(frac_digits) / divisor;
        }
        if (exp_acc != 0) {
            double e = 1.0;
            for (int8_t i = 0; i < exp_acc; ++i) e *= 10.0;
            if (exp_sign < 0) v /= e; else v *= e;
        }
        return sign * v;
    }
};

} // namespace note
