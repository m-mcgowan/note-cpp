#!/usr/bin/env bash
set -euo pipefail

CXX="${CXX:-c++}"
CXXFLAGS="${CXXFLAGS:--std=c++2b}"
INCLUDE="-I $(dirname "$0")/include"

echo "Compiler: $($CXX --version | head -1)"
echo "Flags: $CXXFLAGS $INCLUDE"
echo

# Check each header compiles independently
echo "=== Header compilation ==="
for header in $(find "$(dirname "$0")/include/note" -name '*.hpp' | sort); do
    name=$(basename "$header")
    printf "  %-20s " "$name"
    $CXX $CXXFLAGS $INCLUDE -fsyntax-only "$header" && echo "OK" || { echo "FAIL"; exit 1; }
done

# Build and run the smoke test
echo
echo "=== Smoke test ==="
$CXX $CXXFLAGS $INCLUDE -o /tmp/note-cpp-smoke "$(dirname "$0")/examples/smoke.cpp"
/tmp/note-cpp-smoke
echo "  smoke.cpp: OK"

echo
echo "All checks passed."
