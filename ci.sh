#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

# ── Multi-compiler support ──────────────────────────────────────────────────
# Usage:
#   ./ci.sh                  Run with default compiler (c++ -std=c++2b)
#   ./ci.sh --all-compilers  Run with all available compilers
#   ./ci.sh --coverage       Build with coverage instrumentation and generate report
#   CXX=g++-13 ./ci.sh       Run with a specific compiler
#
# --all-compilers discovers compilers matching the CI matrix (g++-13, clang++-18) plus
# any GCC/Clang versions installed via Homebrew on macOS.

LLVM_COV="${LLVM_COV:-$(xcrun --find llvm-cov 2>/dev/null || echo llvm-cov)}"
LLVM_PROFDATA="${LLVM_PROFDATA:-$(xcrun --find llvm-profdata 2>/dev/null || echo llvm-profdata)}"

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
                if ! git diff --quiet -- "$ROOT/include/note/api/" "$ROOT/include/note/api.hpp" "$ROOT/include/note/api_context.hpp" "$ROOT/tests/test_samples.cpp" "$ROOT/tests/test_api_context.cpp" "$ROOT/tests/test_endpoint_coverage.cpp"; then
                    echo "ERROR: Generated files are out of date. Run the generator and commit."
                    git diff --stat -- "$ROOT/include/note/api/" "$ROOT/include/note/api.hpp" "$ROOT/include/note/api_context.hpp" "$ROOT/tests/test_samples.cpp" "$ROOT/tests/test_api_context.cpp" "$ROOT/tests/test_endpoint_coverage.cpp"
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
        "$ROOT/tests/test_json_buf.cpp" \
        "$ROOT/tests/test_property_functor.cpp" \
        "$ROOT/tests/test_transport_crc32.cpp" \
        "$ROOT/tests/test_transport_serial.cpp" \
        "$ROOT/tests/test_transport_i2c.cpp" \
        "$ROOT/tests/test_notecard.cpp" \
        "$ROOT/tests/test_api_context.cpp" \
        "$ROOT/tests/test_endpoint_coverage.cpp"
    /tmp/note-cpp-tests
    echo "  tests: OK"

    # Version gating tests
    echo
    echo "=== Version gating ==="
    # Warn mode: version-gated fields produce deprecation warnings
    printf "  %-40s " "warn mode"
    WARN_OUT=$($CXX $CXXFLAGS $INCLUDE -fsyntax-only -x c++ - <<'VEOF' 2>&1 || true
#define NOTE_API_VERSION NOTE_VERSION(3, 0, 0)
#include <note/api/hub_set.hpp>
void test() { note::api::HubSet req; req.off = true; }
VEOF
    )
    if echo "$WARN_OUT" | grep -q 'deprecated.*requires firmware'; then
        echo "OK"
    else
        echo "FAIL (expected deprecation warning)"
        echo "$WARN_OUT"
        exit 1
    fi

    # Strict mode: version-gated fields are compiled out
    printf "  %-40s " "strict mode"
    STRICT_OUT=$($CXX $CXXFLAGS $INCLUDE -fsyntax-only -x c++ - <<'VEOF' 2>&1 || true
#define NOTE_API_VERSION NOTE_VERSION(3, 0, 0)
#define NOTE_API_STRICT
#include <note/api/hub_set.hpp>
void test() { note::api::HubSet req; req.off = true; }
VEOF
    )
    if echo "$STRICT_OUT" | grep -q "no member named 'off'"; then
        echo "OK"
    else
        echo "FAIL (expected compile error for gated field)"
        echo "$STRICT_OUT"
        exit 1
    fi

    # Latest version: no warnings
    printf "  %-40s " "latest (no warnings)"
    if $CXX $CXXFLAGS $INCLUDE -Werror -fsyntax-only -x c++ - <<'VEOF' 2>&1; then
#include <note/api/hub_set.hpp>
void test() { note::api::HubSet req; req.off = true; }
VEOF
        echo "OK"
    else
        echo "FAIL (unexpected warnings at latest version)"
        exit 1
    fi

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

run_coverage_clang() {
    local CXX="${CXX:-c++}"
    local CXXFLAGS="${CXXFLAGS:--std=c++2b}"
    local INCLUDE="-I $ROOT/include"
    local OUT_DIR="${ROOT}/coverage"
    local BINARY="/tmp/note-cpp-tests-cov"
    local PROFRAW="/tmp/note-cpp-tests.profraw"
    local PROFDATA="/tmp/note-cpp-tests.profdata"

    echo "=== Coverage build (clang — see docs/coverage.md for accuracy caveats) ==="
    LLVM_PROFILE_FILE="$PROFRAW" \
        $CXX $CXXFLAGS -fprofile-instr-generate -fcoverage-mapping \
        $INCLUDE -I "$ROOT/tests" -o "$BINARY" \
        "$ROOT/tests/test_main.cpp" \
        "$ROOT/tests/test_wire_format.cpp" \
        "$ROOT/tests/test_samples.cpp" \
        "$ROOT/tests/test_body.cpp" \
        "$ROOT/tests/test_json_buf.cpp" \
        "$ROOT/tests/test_property_functor.cpp" \
        "$ROOT/tests/test_transport_crc32.cpp" \
        "$ROOT/tests/test_transport_serial.cpp" \
        "$ROOT/tests/test_transport_i2c.cpp" \
        "$ROOT/tests/test_notecard.cpp" \
        "$ROOT/tests/test_api_context.cpp" \
        "$ROOT/tests/test_endpoint_coverage.cpp"
    LLVM_PROFILE_FILE="$PROFRAW" "$BINARY"

    "$LLVM_PROFDATA" merge -sparse "$PROFRAW" -o "$PROFDATA"

    mkdir -p "$OUT_DIR"
    "$LLVM_COV" export "$BINARY" \
        --instr-profile="$PROFDATA" \
        --format=lcov \
        --ignore-filename-regex="tests/" \
        "${ROOT}/include/note" \
        > "$OUT_DIR/coverage.lcov"

    echo "=== Coverage summary (clang — consteval false positives inflate miss counts) ==="
    "$LLVM_COV" report "$BINARY" \
        --instr-profile="$PROFDATA" \
        --ignore-filename-regex="tests/" \
        "${ROOT}/include/note"

    genhtml "$OUT_DIR/coverage.lcov" \
        --output-directory "$OUT_DIR/html" \
        --title "note-cpp coverage (clang)" \
        --quiet

    echo "  HTML report: ${OUT_DIR}/html/index.html"
    echo
}

run_coverage() {
    # GCC is required for accurate coverage. It correctly marks consteval functions
    # as non-executable and excludes them from metrics. Clang source-based coverage
    # (-fprofile-instr-generate) has a known false-positive where 'static consteval'
    # member functions appear as uncovered, inflating miss counts. Additionally,
    # 'lcov --capture' applies LCOV_EXCL markers at the capture stage, excluding
    # both line and function entries — genhtml-only processing misses function entries.
    #
    # Install GCC: 'brew install gcc' (macOS) or 'apt-get install g++-13' (Ubuntu).
    # If you use clang for coverage, expect ~12% inflated function miss counts.

    # lcov 2.x required: version 1.x has an internal .gcno format parser that does
    # not support the format produced by GCC 13+, causing "Overlong record" errors.
    # Install: 'brew install lcov' (macOS) or 'apt-get install lcov' (Ubuntu 24.04+).
    if command -v lcov >/dev/null 2>&1; then
        local lcov_major
        lcov_major=$(lcov --version 2>&1 | grep -oE '[0-9]+' | head -1)
        if [ "${lcov_major:-0}" -lt 2 ] 2>/dev/null; then
            echo "ERROR: lcov 2.x or newer is required (found: $(lcov --version 2>&1 | head -1))."
            echo "       lcov 1.x cannot parse GCC 13+ .gcno format files."
            echo "       Upgrade: 'brew upgrade lcov' (macOS) or 'apt-get install lcov' (Ubuntu 24.04+)."
            exit 1
        fi
    fi

    # lcov 2.0+ is required: lcov 1.x cannot parse GCC 13's .gcno format (produces
    # empty reports silently). Check version before proceeding.
    if command -v lcov >/dev/null 2>&1; then
        local lcov_ver
        lcov_ver=$(lcov --version 2>&1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
        local lcov_major="${lcov_ver%%.*}"
        if [ "${lcov_major:-0}" -lt 2 ] 2>/dev/null; then
            echo "ERROR: lcov ${lcov_ver} is too old. lcov 2.0+ is required for GCC 13 .gcno support."
            echo "       Install: 'brew upgrade lcov' (macOS) or 'apt-get install lcov' (Ubuntu 24.04+)."
            exit 1
        fi
    else
        echo "ERROR: lcov not found. Install: 'brew install lcov' (macOS) or 'apt-get install lcov' (Ubuntu)."
        exit 1
    fi

    local GCC=""
    for g in g++-14 g++-13; do
        for p in /opt/homebrew/bin /usr/local/bin /usr/bin ""; do
            local candidate="${p:+$p/}$g"
            if command -v "$candidate" >/dev/null 2>&1; then
                GCC="$candidate"
                break 2
            fi
        done
    done
    # Fall back to bare g++ only if it's actually GCC (not Apple Clang).
    if [ -z "$GCC" ] && command -v g++ >/dev/null 2>&1; then
        if g++ --version 2>&1 | grep -q GCC; then
            GCC="g++"
        fi
    fi
    if [ -z "$GCC" ]; then
        echo "WARNING: GCC not found; falling back to clang source-based coverage."
        echo "         Function coverage will be understated: clang incorrectly marks"
        echo "         'static consteval' member functions as uncovered (false positives)."
        echo "         See docs/coverage.md for details."
        echo "         Install real GCC: 'brew install gcc' (macOS) or 'apt-get install g++-13' (Ubuntu)."
        echo
        run_coverage_clang
        return
    fi

    local CXXFLAGS="-std=c++23"
    local INCLUDE="-I $ROOT/include"
    local OUT_DIR="${ROOT}/coverage"
    local BUILD_DIR="/tmp/note-cpp-cov-build"
    local BINARY="/tmp/note-cpp-tests-cov"

    # lcov must use the gcov matching the compiler version (e.g. gcov-13 with g++-13).
    # The default 'gcov' is typically an older version that can't read newer .gcno files.
    local gcc_ver="${GCC##*-}"   # "13" from "/usr/local/bin/g++-13" or "g++-13"
    local gcc_dir
    gcc_dir="$(dirname "$(command -v "$GCC")")"
    local GCOV="${gcc_dir}/gcov-${gcc_ver}"
    if ! command -v "$GCOV" >/dev/null 2>&1; then
        GCOV="gcov-${gcc_ver}"
    fi
    if ! command -v "$GCOV" >/dev/null 2>&1; then
        GCOV="gcov"
    fi

    echo "=== Coverage build ($(${GCC} --version | head -1)) ==="
    rm -rf "$BUILD_DIR" && mkdir -p "$BUILD_DIR"

    local SRCS=(
        test_main test_wire_format test_samples test_body
        test_json_buf test_property_functor
        test_transport_crc32 test_transport_serial test_transport_i2c
        test_notecard test_api_context test_endpoint_coverage
    )
    for name in "${SRCS[@]}"; do
        "$GCC" $CXXFLAGS --coverage -fprofile-arcs $INCLUDE -I "$ROOT/tests" \
            -c "$ROOT/tests/${name}.cpp" -o "$BUILD_DIR/${name}.o"
    done
    "$GCC" --coverage -fprofile-arcs -o "$BINARY" "$BUILD_DIR"/*.o
    "$BINARY"

    mkdir -p "$OUT_DIR"
    echo
    echo "=== Collecting coverage data ==="
    # lcov --capture reads source files and applies LCOV_EXCL markers, correctly
    # excluding both line and function entries (unlike genhtml-only processing).
    lcov --capture \
        --directory "$BUILD_DIR" \
        --gcov-tool "$GCOV" \
        --rc branch_coverage=1 \
        --rc no_exception_branch=1 \
        --ignore-errors mismatch \
        --output-file "$OUT_DIR/coverage-raw.lcov" \
        --quiet
    # Keep only our headers; strip third_party.
    lcov --extract "$OUT_DIR/coverage-raw.lcov" "*/include/note/*" \
        --rc branch_coverage=1 \
        --output-file "$OUT_DIR/coverage-filtered.lcov" \
        --quiet
    lcov --remove "$OUT_DIR/coverage-filtered.lcov" "*/third_party/*" \
        --rc branch_coverage=1 \
        --output-file "$OUT_DIR/coverage.lcov" \
        --quiet

    echo
    echo "=== Coverage summary ==="
    lcov --summary "$OUT_DIR/coverage.lcov" --rc branch_coverage=1

    echo
    echo "=== Generating HTML report → ${OUT_DIR}/html ==="
    genhtml "$OUT_DIR/coverage.lcov" \
        --output-directory "$OUT_DIR/html" \
        --title "note-cpp coverage" \
        --branch-coverage \
        --quiet

    echo "  HTML report: ${OUT_DIR}/html/index.html"
    echo
}

case "${1:-}" in
    --coverage)
        run_coverage
        ;;
    --all-compilers)
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
        ;;
    *)
        run_ci "${CXX:-c++}" "${CXXFLAGS:--std=c++2b}"
        ;;
esac
