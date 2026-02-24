#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstdint>

namespace note {

/// Malloc/free function signatures (same as note-c's mallocFn/freeFn).
using MallocFn = void* (*)(size_t);
using FreeFn = void (*)(void*);

/// Heap profiling snapshot.
struct HeapStats {
    size_t current_bytes = 0;    // currently allocated
    size_t peak_bytes = 0;       // high-water mark
    uint32_t alloc_count = 0;    // total allocations
    uint32_t free_count = 0;     // total frees
    uint32_t active_count = 0;   // alloc_count - free_count
};

/// Pluggable allocator with optional profiling.
///
/// Default: delegates to std::malloc/std::free.
/// Call set_hooks() to redirect, and set_profiling() to enable tracking.
class Allocator {
public:
    static void set_hooks(MallocFn m, FreeFn f) {
        instance().malloc_ = m;
        instance().free_ = f;
    }

    static void set_profiling(bool enabled) {
        instance().profiling_ = enabled;
    }

    static void reset_stats() {
        instance().stats_ = {};
    }

    static HeapStats stats() {
        return instance().stats_;
    }

    static void* alloc(size_t size) {
        auto& self = instance();
        void* p = self.malloc_(size);
        if (p && self.profiling_) {
            self.stats_.alloc_count++;
            self.stats_.active_count++;
            self.stats_.current_bytes += size;
            if (self.stats_.current_bytes > self.stats_.peak_bytes) {
                self.stats_.peak_bytes = self.stats_.current_bytes;
            }
        }
        return p;
    }

    static void free(void* p, size_t size = 0) {
        auto& self = instance();
        if (p && self.profiling_) {
            self.stats_.free_count++;
            if (self.stats_.active_count > 0) self.stats_.active_count--;
            if (self.stats_.current_bytes >= size) {
                self.stats_.current_bytes -= size;
            } else {
                self.stats_.current_bytes = 0;
            }
        }
        self.free_(p);
    }

private:
    MallocFn malloc_ = std::malloc;
    FreeFn free_ = std::free;
    bool profiling_ = false;
    HeapStats stats_{};

    static Allocator& instance() {
        static Allocator a;
        return a;
    }
};

/// RAII scope guard that captures heap stats before/after a block.
struct HeapSnapshot {
    HeapStats before;
    HeapStats after;

    static HeapSnapshot begin() {
        HeapSnapshot s;
        s.before = Allocator::stats();
        return s;
    }

    void end() {
        after = Allocator::stats();
    }

    size_t bytes_allocated() const { return after.peak_bytes - before.peak_bytes; }
    uint32_t allocations() const { return after.alloc_count - before.alloc_count; }
    uint32_t frees() const { return after.free_count - before.free_count; }
};

} // namespace note
