// Host-only: provides doctest's runtime implementation + main().
// Linked into note-cpp-integration-backends (and Phase 2's unified test binary).
// NOT compiled on device — firmware's test/main.cpp carries DOCTEST_CONFIG_IMPLEMENT.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

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
