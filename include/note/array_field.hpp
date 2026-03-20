#pragma once

/// Fixed-capacity array field for JSON array-type request properties.
///
/// Stores up to N items and serializes them as a JSON array in build().
///
///   req.files.add("data.qi").add("my-settings.db");
///   req.files = {"data.qi", "my-settings.db"};
///   req.files({"data.qi", "my-settings.db"});
///   // → "files":["data.qi","my-settings.db"]
///
/// @tparam T   Element type (typically note::string_view).
/// @tparam N   Maximum number of elements (compile-time constant).

#include <cstddef>
#include <initializer_list>
#include "json.hpp"
#include "types.hpp"

namespace note {

template<typename T, std::size_t N>
class ArrayField {
public:
    ArrayField() = default;

    ArrayField(std::initializer_list<T> items) {
        for (auto& item : items)
            add(item);
    }

    ArrayField& operator=(std::initializer_list<T> items) {
        count_ = 0;
        for (auto& item : items)
            add(item);
        return *this;
    }

    ArrayField& operator()(std::initializer_list<T> items) {
        return operator=(items);
    }

    ArrayField& add(T item) {
        if (count_ < N) items_[count_++] = item;
        return *this;
    }

    void clear() { count_ = 0; }

    explicit operator bool() const { return count_ > 0; }
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }

    void write_to(JsonBuilder& b, string_view key) const {
        b.begin_array(key);
        for (std::size_t i = 0; i < count_; ++i)
            b.add_element(items_[i]);
        b.end_array();
    }

private:
    T items_[N]{};
    std::size_t count_{};
};

} // namespace note
