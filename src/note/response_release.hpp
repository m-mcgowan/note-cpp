// Helpers used by generated Response destructors to release interned strings
// back to the Allocator that minted them.
//
// Codegen emits a Response dtor of the shape:
//
//     ~Response() {
//         if (!alloc_) return;
//         note::detail::release_string_fields(*alloc_, &first_string_field_,
//                                             n_string_fields);
//         note::detail::release_string_array(*alloc_, files);   // per array field
//         note::detail::release_string_array(*alloc_, names);   // (zero or more)
//     }
//
// Two helpers, not one, because string-array fields are
// `ResponseArray<string_view, N>` and N varies per endpoint — they can't be
// walked as a flat block of homogeneous arrays. The string-singleton block
// IS homogeneous and walks as a simple pointer loop.
#pragma once

#include <note/allocator.hpp>
#include <note/field.hpp>
#include <note/response_array.hpp>
#include <note/types.hpp>

#include <cstddef>

namespace note {

// Move-only owner of an Allocator value, used by every generated Response
// to track which Allocator (if any) minted the strings it holds. Captures
// the Allocator BY VALUE — necessary because the Notecard's internal
// allocator storage can be mutated by callers (e.g. via
// `nc.execute(req, temp_alloc)` which swaps then restores). A pointer
// into that storage would dangle on restore; an embedded value stays
// valid for the Response's lifetime.
//
// Move semantics: transferring the AllocatorRef sets the source's
// "present" flag to false so only the live owner runs cleanup in
// `Response::~Response()`.
class AllocatorRef {
    Allocator alloc_{};
    bool present_ = false;
public:
    AllocatorRef() = default;
    explicit AllocatorRef(const Allocator& a) noexcept : alloc_(a), present_(true) {}

    AllocatorRef(AllocatorRef&& o) noexcept
        : alloc_(o.alloc_), present_(o.present_) { o.present_ = false; }
    AllocatorRef& operator=(AllocatorRef&& o) noexcept {
        if (this != &o) {
            alloc_ = o.alloc_;
            present_ = o.present_;
            o.present_ = false;
        }
        return *this;
    }
    AllocatorRef(const AllocatorRef&) = delete;
    AllocatorRef& operator=(const AllocatorRef&) = delete;

    explicit operator bool() const noexcept { return present_; }
    Allocator& operator*() noexcept { return alloc_; }
    const Allocator& operator*() const noexcept { return alloc_; }
    Allocator* operator->() noexcept { return &alloc_; }
    const Allocator* operator->() const noexcept { return &alloc_; }

    // Wire-in point used by execute() paths to attach the configured
    // Allocator after the Response has been parsed.
    void reset(const Allocator& a) noexcept { alloc_ = a; present_ = true; }
};

namespace detail {

inline void deallocate_if_present(Allocator& alloc, string_view sv) {
    if (sv.empty()) return;
    // intern() allocated sv.size() + 1 bytes (the +1 is the null terminator
    // that makes .data() a valid C string). Return the same size on free.
    alloc.deallocate(const_cast<char*>(sv.data()), sv.size() + 1);
}

// Walk a contiguous run of N ResponseField<string_view> objects, freeing each.
// `first` is the address of the first field; the rest are reachable by
// pointer arithmetic because codegen emits the string-view fields as adjacent
// members under one access specifier (standard-layout, same type → defined).
inline void release_string_fields(Allocator& alloc,
                                  ResponseField<string_view>* first,
                                  size_t count) {
    for (size_t i = 0; i < count; ++i) {
        deallocate_if_present(alloc, first[i].value());
    }
}

// Walk all elements of a ResponseArray of string-shaped elements (either
// `note::string_view` or `note::printable_string_view`, which is the
// Arduino-friendly wrapper codegen uses for string-array fields).
// Templated on the element type so both shapes resolve without overloads.
template<typename SV, size_t N>
inline void release_string_array(Allocator& alloc, ResponseArray<SV, N>& arr) {
    for (auto& el : arr) {
        deallocate_if_present(alloc, string_view{el.data(), el.size()});
    }
}

} // namespace detail
} // namespace note
