#pragma once

/// Type-safe builder for Notecard voltage-variable strings.
///
/// The Notecard's `vinbound`/`voutbound` fields accept a semicolon-delimited
/// string like `"usb:5;high:15;normal:60;low:240;dead:0"` mapping USB power
/// levels to durations in minutes.
///
///   note::VoltageVariable vv;
///   vv.usb(5).high(15).normal(60).low(240).dead(0);
///   req.voutbound(vv);   // implicit string_view conversion
///
/// Only levels that are explicitly set are emitted.

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace note {

class VoltageVariable {
public:
    VoltageVariable& usb(int32_t minutes) { return append("usb", minutes); }
    VoltageVariable& high(int32_t minutes) { return append("high", minutes); }
    VoltageVariable& normal(int32_t minutes) { return append("normal", minutes); }
    VoltageVariable& low(int32_t minutes) { return append("low", minutes); }
    VoltageVariable& dead(int32_t minutes) { return append("dead", minutes); }

    std::string_view str() const { return {buf_, len_}; }
    operator std::string_view() const { return str(); }
    explicit operator bool() const { return len_ > 0; }

    bool empty() const { return len_ == 0; }

private:
    VoltageVariable& append(const char* level, int32_t val) {
        if (len_ >= sizeof(buf_)) return *this;
        // Add semicolon separator between entries
        if (len_ > 0) buf_[len_++] = ';';
        int n = std::snprintf(buf_ + len_, sizeof(buf_) - len_, "%s:%d",
                              level, static_cast<int>(val));
        if (n > 0 && len_ + static_cast<size_t>(n) < sizeof(buf_))
            len_ += static_cast<size_t>(n);
        return *this;
    }

    char buf_[64]{};
    size_t len_ = 0;
};

} // namespace note
