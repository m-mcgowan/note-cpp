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
#include <note/note_config.hpp>
#include <note/response_array.hpp>
#include <note/types.hpp>

#include <cstddef>

namespace note {

#if !NOTE_NO_RESPONSE_RAII

#if NOTE_SINGLETON
namespace detail {
    /// Singleton allocator slot. Set by Notecard and StaticNotecard ctors
    /// / `set_allocator()` / `clear_allocator()`; read by every generated
    /// Response destructor under SINGLETON in place of a per-Response
    /// Allocator copy.
    ///
    /// Captured BY VALUE rather than by pointer so the slot survives any
    /// Notecard moves / returns-by-value (factory patterns) without
    /// requiring custom move ctors to chase the storage. Cost: one
    /// Allocator-value (≈ 8 B on AVR, ≈ 32 B on 64-bit hosts) in BSS,
    /// independent of how many Responses are alive.
    ///
    /// `present_` distinguishes "no allocator configured" from a
    /// default-heap allocator (which has the same byte pattern as the
    /// zero-initialized BSS value).
    inline ::note::Allocator g_singleton_allocator{};
    inline bool g_singleton_allocator_present = false;
}
#endif

// Move-only owner of allocator information for a parsed Response.
//
// Two shapes:
//
// * Default build: captures the Allocator BY VALUE — needed because the
//   Notecard's internal allocator storage can be mutated by callers (e.g.
//   via `nc.execute(req, temp_alloc)` which swaps then restores). A
//   pointer into that storage would dangle on restore; an embedded value
//   stays valid for the Response's lifetime.
//
// * `NOTE_SINGLETON=1`: stores only a 1-byte "owns" flag. The actual
//   Allocator is read from `note::detail::g_singleton_allocator` at
//   destruction time — there's only one Notecard in the program, so the
//   global is unambiguous and the per-Response Allocator copy is pure
//   overhead. The per-call `nc.execute(req, temp_alloc)` overload is
//   gated out under SINGLETON because its swap-and-restore semantics
//   can't be reconciled with a global pointer when Responses outlive
//   the call.
//
// Move semantics: transferring the AllocatorRef sets the source's
// "present" flag to false so only the live owner runs cleanup in
// `Response::~Response()`.
class AllocatorRef {
#if NOTE_SINGLETON
    bool present_ = false;
public:
    AllocatorRef() = default;
    explicit AllocatorRef(const Allocator&) noexcept : present_(true) {}

    AllocatorRef(AllocatorRef&& o) noexcept : present_(o.present_) { o.present_ = false; }
    AllocatorRef& operator=(AllocatorRef&& o) noexcept {
        if (this != &o) {
            present_ = o.present_;
            o.present_ = false;
        }
        return *this;
    }
    AllocatorRef(const AllocatorRef&) = delete;
    AllocatorRef& operator=(const AllocatorRef&) = delete;

    /// True only when the AllocatorRef was attached AND a singleton
    /// allocator is currently registered.
    explicit operator bool() const noexcept {
        return present_ && note::detail::g_singleton_allocator_present;
    }
    Allocator& operator*() noexcept { return note::detail::g_singleton_allocator; }
    const Allocator& operator*() const noexcept { return note::detail::g_singleton_allocator; }
    Allocator* operator->() noexcept { return &note::detail::g_singleton_allocator; }
    const Allocator* operator->() const noexcept { return &note::detail::g_singleton_allocator; }

    // Wire-in point used by execute() paths to attach the configured
    // Allocator after the Response has been parsed. The argument is
    // ignored under SINGLETON — the global supplies the actual Allocator.
    void reset(const Allocator&) noexcept { present_ = true; }
#else
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
#endif
};

#endif // !NOTE_NO_RESPONSE_RAII

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
