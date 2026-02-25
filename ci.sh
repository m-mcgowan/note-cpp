#!/usr/bin/env bash
set -euo pipefail

ROOT="$(dirname "$0")"

# ── Multi-compiler support ──────────────────────────────────────────────────
# Usage:
#   ./ci.sh                  Run with default compiler (c++ -std=c++2b)
#   ./ci.sh --all-compilers  Run with all available compilers
#   CXX=g++-13 ./ci.sh       Run with a specific compiler
#
# --all-compilers discovers compilers matching the CI matrix (g++-13, clang++-18) plus
# any GCC/Clang versions installed via Homebrew on macOS.

run_ci() {
    local CXX="$1"
    local CXXFLAGS="$2"
    local INCLUDE="-I $ROOT/include"

    echo "════════════════════════════════════════════════════════════════"
    echo "Compiler: $($CXX --version | head -1)"
    echo "Flags: $CXXFLAGS $INCLUDE"
    echo "════════════════════════════════════════════════════════════════"
    echo

    # Code generation (first compiler only)
    if [ "${CODEGEN_DONE:-}" != "1" ]; then
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
        export CODEGEN_DONE=1
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
    echo "All checks passed for $CXX."
    echo
}

discover_compilers() {
    local compilers=()

    # Always include the default system compiler
    compilers+=("c++:-std=c++2b")

    # Look for GCC versions (Homebrew on macOS, system on Linux)
    for gxx in /usr/local/bin/g++-* /opt/homebrew/bin/g++-* /usr/bin/g++-13 /usr/bin/g++-14; do
        if [ -x "$gxx" ]; then
            # GCC 13+ required for C++23
            local ver
            ver=$("$gxx" -dumpversion 2>/dev/null | cut -d. -f1)
            if [ "${ver:-0}" -ge 13 ] 2>/dev/null; then
                compilers+=("$gxx:-std=c++23")
            fi
        fi
    done

    # Look for Clang versions with libc++ (Linux CI style)
    for clangxx in /usr/bin/clang++-18 /usr/bin/clang++-19; do
        if [ -x "$clangxx" ]; then
            compilers+=("$clangxx:-std=c++23 -stdlib=libc++")
        fi
    done

    # Deduplicate by compiler basename
    local seen=""
    for entry in "${compilers[@]}"; do
        local cxx="${entry%%:*}"
        local base
        base=$(basename "$cxx")
        if [[ "$seen" != *"$base"* ]]; then
            seen="$seen $base"
            echo "$entry"
        fi
    done
}

if [ "${1:-}" = "--all-compilers" ]; then
    echo "Discovering compilers..."
    FAILED=0
    while IFS= read -r entry; do
        cxx="${entry%%:*}"
        flags="${entry#*:}"
        run_ci "$cxx" "$flags" || FAILED=1
    done < <(discover_compilers)

    if [ "$FAILED" -ne 0 ]; then
        echo "SOME COMPILERS FAILED"
        exit 1
    fi
    echo "All compilers passed."
else
    run_ci "${CXX:-c++}" "${CXXFLAGS:--std=c++2b}"
fi
