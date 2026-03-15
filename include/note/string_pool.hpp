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
public:
    explicit StringPool(Allocator alloc) : alloc_(alloc) {}

    /// Copy sv's data into pool storage, return a view into the copy.
    string_view intern(string_view sv) {
        if (sv.empty()) return {};
        auto* p = static_cast<char*>(alloc_.allocate(sv.size()));
        std::memcpy(p, sv.data(), sv.size());
        return {p, sv.size()};
    }
};

} // namespace note
