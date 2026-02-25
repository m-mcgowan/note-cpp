#!/usr/bin/env bash
set -euo pipefail

ROOT="$(dirname "$0")"
CXX="${CXX:-c++}"
CXXFLAGS="${CXXFLAGS:--std=c++2b}"
INCLUDE="-I $ROOT/include"

echo "Compiler: $($CXX --version | head -1)"
echo "Flags: $CXXFLAGS $INCLUDE"
echo

# Code generation (requires Python 3 + jinja2)
PYTHON=python3
if [ -f "$ROOT/.venv/bin/python3" ]; then
    PYTHON="$ROOT/.venv/bin/python3"
fi
if command -v "$PYTHON" >/dev/null 2>&1; then
    echo "=== Code generation ==="
    "$PYTHON" "$ROOT/tools/codegen/generate.py" "$ROOT/notecard-api.openapi.json" \
        -o "$ROOT/include/note/api" \
        --umbrella "$ROOT/include/note/api.hpp" \
        --api-context "$ROOT/include/note/api_context.hpp" \
        --test-dir "$ROOT/tests"

    # Check that generated files are up to date (CI only)
    if [ "${CI:-}" = "true" ]; then
        if ! git diff --quiet -- "$ROOT/include/note/api/" "$ROOT/include/note/api.hpp" "$ROOT/include/note/api_context.hpp" "$ROOT/tests/test_samples.cpp"; then
            echo "ERROR: Generated files are out of date. Run the generator and commit."
            git diff --stat -- "$ROOT/include/note/api/" "$ROOT/include/note/api.hpp" "$ROOT/include/note/api_context.hpp" "$ROOT/tests/test_samples.cpp"
            exit 1
        fi
        echo "  Generated files are up to date."
    fi
    echo
fi

# Check each header compiles independently
echo "=== Header compilation ==="
for header in $(find "$ROOT/include/note" -name '*.hpp' | sort); do
    name=$(basename "$header")
    printf "  %-40s " "$name"
    $CXX $CXXFLAGS $INCLUDE -fsyntax-only "$header" && echo "OK" || { echo "FAIL"; exit 1; }
done

# Build and run unit tests
echo
echo "=== Unit tests ==="
$CXX $CXXFLAGS $INCLUDE -I "$ROOT/tests" -o /tmp/note-cpp-tests \
    "$ROOT/tests/test_main.cpp" \
    "$ROOT/tests/test_wire_format.cpp" \
    "$ROOT/tests/test_samples.cpp" \
    "$ROOT/tests/test_body.cpp" \
    "$ROOT/tests/test_json_buf.cpp"
/tmp/note-cpp-tests
echo "  tests: OK"

# Build and run the smoke test
echo
echo "=== Smoke test ==="
$CXX $CXXFLAGS $INCLUDE -o /tmp/note-cpp-smoke "$ROOT/examples/smoke.cpp"
/tmp/note-cpp-smoke
echo "  smoke.cpp: OK"

echo
echo "All checks passed."
