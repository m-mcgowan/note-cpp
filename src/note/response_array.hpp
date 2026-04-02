#pragma once

/// @file response_array.hpp
/// Fixed-capacity array for parsed response fields.
///
/// Holds up to N elements inline (no heap allocation). Used by generated
/// response types for JSON array fields like card.attn's "files".
///
///   ResponseArray<string_view, 16> files;
///   files.add("data.qi");
///   for (auto& f : files) { ... }

#include <cstddef>

namespace note {

template<typename T, size_t N>
struct ResponseArray {
    T items_[N]{};
    size_t count_ = 0;

    void add(const T& item) { if (count_ < N) items_[count_++] = item; }
    void clear() { count_ = 0; }

    size_t size() const { return count_; }
    size_t capacity() const { return N; }
    bool empty() const { return count_ == 0; }

    const T& operator[](size_t i) const { return items_[i]; }
    const T* begin() const { return items_; }
    const T* end() const { return items_ + count_; }
    T* begin() { return items_; }
    T* end() { return items_ + count_; }

    bool operator==(const ResponseArray& o) const {
        if (count_ != o.count_) return false;
        for (size_t i = 0; i < count_; ++i)
            if (items_[i] != o.items_[i]) return false;
        return true;
    }
    bool operator!=(const ResponseArray& o) const { return !(*this == o); }
};

} // namespace note
