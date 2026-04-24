// Host-only: provides doctest's runtime implementation + main().
// Linked into note-cpp-integration-backends (and Phase 2's unified test binary).
// NOT compiled on device — firmware's test/main.cpp carries DOCTEST_CONFIG_IMPLEMENT.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
