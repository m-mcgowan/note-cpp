// StringPool: copies string_view data into allocator-backed storage.
//
// Response string_views initially point into the transport buffer, which is
// invalidated on the next execute() call. StringPool.intern() copies them
// into arena-backed (or heap-backed) storage so they survive buffer reuse.
#pragma once

#include <note/allocator.hpp>
#include <note/types.hpp>

#include <cstring>

namespace note {

class StringPool {
    Allocator alloc_;
    bool exhausted_ = false;
public:
    explicit StringPool(Allocator alloc) : alloc_(alloc) {}

    /// Access the underlying allocator (for arena-backed buffer growth).
    Allocator& allocator() { return alloc_; }

    /// Copy sv's data into pool storage, return a view into the copy.
    string_view intern(string_view sv) {
        if (sv.empty()) return {};
        auto* p = static_cast<char*>(alloc_.allocate(sv.size()));
        if (!p) { exhausted_ = true; return {}; }
        std::memcpy(p, sv.data(), sv.size());
        return {p, sv.size()};
    }

    /// Returns true if any intern() call failed due to allocation exhaustion.
    bool exhausted() const { return exhausted_; }
};

} // namespace note
