// Host-only: provides doctest's runtime implementation + main().
// Linked into note-cpp-integration-backends (and Phase 2's unified test binary).
// NOT compiled on device — firmware's test/main.cpp carries DOCTEST_CONFIG_IMPLEMENT.

#define DOCTEST_CONFIG_IMPLEMENT

// Vendored doctest.h trips -Wzero-as-null-pointer-constant on the macOS
// sysctl() call in its debugger-detection helper. Silence it here rather
// than patching the upstream header.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif

#include <doctest.h>

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include "common/alloc_counter.hpp"
#include <cstdio>

int main(int argc, char** argv) {
    // Snapshot static-init heap usage before doctest dispatches anything else.
    // Useful for sizing up TEST_CASE registration overhead on memory-constrained
    // targets — a key Phase 2 question is whether the full ~1800-case suite
    // fits in the ESP32-S3's ~250 KB heap.
    auto init_n = note_tests::g_init.count.load(std::memory_order_relaxed);
    auto init_b = note_tests::g_init.bytes.load(std::memory_order_relaxed);
    std::printf("[init-phase] %zu allocations, %zu bytes total (%.1f bytes/alloc)\n",
                init_n, init_b, init_n ? double(init_b) / double(init_n) : 0.0);
    note_tests::init_phase_freeze();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
