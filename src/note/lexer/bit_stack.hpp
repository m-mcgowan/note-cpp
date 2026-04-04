#pragma once

/// @file bit_stack.hpp
/// BitStack — compact nesting tracker for JSON lexing.
///
/// Each bit represents one nesting level: 1 = object, 0 = array.
/// Validates that } closes { and ] closes [.
///
/// Template parameter Word controls capacity:
///   BitStack<uint32_t>  — 32 levels (5 bytes)
///   BitStack<uint64_t>  — 64 levels (9 bytes)

#include <cstdint>

namespace note {

template<typename Word = uint32_t>
struct BitStack {
    Word bits = 0;
    uint8_t depth = 0;

    static constexpr uint8_t max_depth = sizeof(Word) * 8;

    bool push_object() {
        if (depth >= max_depth) return false;
        bits |= (Word{1} << depth);
        ++depth;
        return true;
    }

    bool push_array() {
        if (depth >= max_depth) return false;
        bits &= ~(Word{1} << depth);
        ++depth;
        return true;
    }

    bool pop_object() {
        if (depth == 0) return false;
        --depth;
        if (!(bits & (Word{1} << depth))) return false;  // was array, not object
        return true;
    }

    bool pop_array() {
        if (depth == 0) return false;
        --depth;
        if (bits & (Word{1} << depth)) return false;  // was object, not array
        return true;
    }

    bool empty() const { return depth == 0; }
    bool in_object() const { return depth > 0 && (bits & (Word{1} << (depth - 1))); }
    bool in_array() const { return depth > 0 && !(bits & (Word{1} << (depth - 1))); }
};

} // namespace note
