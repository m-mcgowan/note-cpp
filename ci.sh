#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

# ── Multi-compiler support ──────────────────────────────────────────────────
# Usage:
#   ./ci.sh                  Quick check: codegen + unit tests (default)
#   ./ci.sh --full           Full CI: headers, examples, version gating, docs
#   ./ci.sh --all-compilers  Full CI with all available compilers
#   ./ci.sh --coverage       Build with coverage instrumentation and generate report
#   ./ci.sh --integrations   Build and run JSON backend integration tests
#   CXX=g++-13 ./ci.sh       Run with a specific compiler
#
# --all-compilers discovers compilers matching the CI matrix (g++-13, clang++-18) plus
# any GCC/Clang versions installed via Homebrew on macOS.

LLVM_COV="${LLVM_COV:-$(xcrun --find llvm-cov 2>/dev/null || echo llvm-cov)}"
LLVM_PROFDATA="${LLVM_PROFDATA:-$(xcrun --find llvm-profdata 2>/dev/null || echo llvm-profdata)}"

_ci_stage_start=0
ci_stage() {
    local now
    now=$(date +%s)
    if [ "$_ci_stage_start" -gt 0 ]; then
        printf "  (%ds)\n" $(( now - _ci_stage_start ))
    fi
    echo
    echo "=== $1 ==="
    _ci_stage_start=$now
}

run_ci() {
    local CXX="$1"
    local CXXFLAGS="$2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror"
    local INCLUDE="-I $ROOT/include"
    local _ci_run_start
    _ci_run_start=$(date +%s)
    _ci_stage_start=0

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
            ci_stage "Code generation"
            "$PYTHON" "$ROOT/tools/codegen/generate.py" "$ROOT/notecard-api.openapi.json" \
                -o "$ROOT/include/note/api" \
                --api "$ROOT/include/note/api.hpp" \
                --test-dir "$ROOT/tests"

            if [ "${CI:-}" = "true" ]; then
                if ! git diff --quiet -- "$ROOT/include/note/api/" "$ROOT/include/note/api.hpp" "$ROOT/tests/test_samples.cpp" "$ROOT/tests/test_api_context.cpp" "$ROOT/tests/test_endpoint_coverage.cpp"; then
                    echo "ERROR: Generated files are out of date. Run the generator and commit."
                    git diff --stat -- "$ROOT/include/note/api/" "$ROOT/include/note/api.hpp" "$ROOT/tests/test_samples.cpp" "$ROOT/tests/test_api_context.cpp" "$ROOT/tests/test_endpoint_coverage.cpp"
                    exit 1
                fi
                echo "  Generated files are up to date."
            fi
            echo
        fi
        export CODEGEN_DONE=1
    fi

    # Check each header compiles independently (parallelized)
    ci_stage "Header compilation"
    find "$ROOT/include/note" -name '*.hpp' \
        -not -path '*/backends/*' -not -path '*/arduino/*' \
        -not -path '*/third_party/*' -not -name 'arduino.hpp' \
    | xargs -P "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
        -I{} sh -c "$CXX $CXXFLAGS $INCLUDE -fsyntax-only {} 2>&1 || echo HEADER_FAIL: {}" \
    | tee /tmp/note-cpp-headers.log
    if grep -q 'HEADER_FAIL:' /tmp/note-cpp-headers.log; then
        echo "FAIL: headers above did not compile"
        grep 'HEADER_FAIL:' /tmp/note-cpp-headers.log
        exit 1
    fi
    echo "  all headers OK"

    # Verify C++17 header compatibility (parallelized, transport/third_party are C++20-only)
    if [ "${CPP17_DONE:-}" != "1" ]; then
        ci_stage "C++17 header compatibility"
        find "$ROOT/include/note" -name '*.hpp' \
            -not -path '*/backends/*' -not -path '*/arduino/*' \
            -not -name 'arduino.hpp' -not -path '*/transport/*' \
            -not -path '*/third_party/*' \
        | xargs -P "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
            -I{} sh -c "$CXX -std=c++17 $INCLUDE -fsyntax-only {} 2>&1 || echo HEADER_FAIL: {}" \
        | tee /tmp/note-cpp-headers17.log
        if grep -q 'HEADER_FAIL:' /tmp/note-cpp-headers17.log; then
            echo "FAIL: C++17 headers above did not compile"
            grep 'HEADER_FAIL:' /tmp/note-cpp-headers17.log
            exit 1
        fi
        echo "  all C++17 headers OK"
        export CPP17_DONE=1
    fi

    # Build and run unit tests
    echo
    ci_stage "Unit tests"
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
        "$ROOT/tests/test_endpoint_coverage.cpp" \
        "$ROOT/tests/test_voltage_variable.cpp" \
        "$ROOT/tests/test_flag_set.cpp" \
        "$ROOT/tests/test_json_sax.cpp" \
        "$ROOT/tests/test_channel.cpp" \
        "$ROOT/tests/test_state_store.cpp" \
        "$ROOT/tests/test_target.cpp" \
        "$ROOT/tests/test_make_api.cpp" \
        "$ROOT/tests/test_units.cpp" \
        "$ROOT/tests/test_connection.cpp" \
        "$ROOT/tests/test_sync.cpp" \
        "$ROOT/tests/test_templates.cpp" \
        "$ROOT/tests/test_attention.cpp" \
        "$ROOT/tests/test_setup.cpp" \
        "$ROOT/tests/test_cobs.cpp" \
        "$ROOT/tests/test_arduino_printable.cpp"
    /tmp/note-cpp-tests
    echo "  tests: OK"

    # Version gating tests
    echo
    ci_stage "Version gating"
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
    if echo "$STRICT_OUT" | grep -q "no member named"; then
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

    # Target filtering tests (C++20 only)
    echo
    ci_stage "Target filtering"
    # Strict mode: unsupported endpoints should fail to compile
    printf "  %-40s " "strict rejects unsupported"
    STRICT_TARGET_OUT=$($CXX $CXXFLAGS $INCLUDE -fsyntax-only -x c++ - <<'TEOF' 2>&1 || true
#include <note/api.hpp>
using LoRaStrict = note::Target<note::Product::LoRa, true>;
void test(note::Api<LoRaStrict>& api) { api.card.sleep(); }
TEOF
    )
    if echo "$STRICT_TARGET_OUT" | grep -qE "constraint|no matching"; then
        echo "OK"
    else
        echo "FAIL (expected compile error for unsupported endpoint on strict target)"
        echo "$STRICT_TARGET_OUT"
        exit 1
    fi

    # Non-strict mode: unsupported endpoints produce deprecation warnings
    printf "  %-40s " "warn for unsupported"
    WARN_TARGET_OUT=$($CXX $CXXFLAGS $INCLUDE -fsyntax-only -x c++ - <<'TEOF' 2>&1 || true
#include <note/api.hpp>
using LoRaWarn = note::Target<note::Product::LoRa, false>;
void test(note::Api<LoRaWarn>& api) { api.card.sleep(); }
TEOF
    )
    if echo "$WARN_TARGET_OUT" | grep -q "deprecated.*not available on this target"; then
        echo "OK"
    else
        echo "FAIL (expected deprecation warning for unsupported endpoint)"
        echo "$WARN_TARGET_OUT"
        exit 1
    fi

    # Supported target: no warnings
    printf "  %-40s " "supported (no warnings)"
    if $CXX $CXXFLAGS $INCLUDE -Werror -fsyntax-only -x c++ - <<'TEOF' 2>&1; then
#include <note/api.hpp>
using WifiTarget = note::Target<note::Product::WiFi>;
void test(note::Api<WifiTarget>& api) { api.card.sleep(); api.hub.set(); }
TEOF
        echo "OK"
    else
        echo "FAIL (unexpected warnings for supported target)"
        exit 1
    fi

    # Build all examples
    echo
    ci_stage "Examples"
    for ex in $(find "$ROOT/examples" -name '*.cpp' -not -path '*/arduino-migration/*' -not -path '*/binary-size-comparison/*' | sort); do
        name=${ex#$ROOT/examples/}
        printf "  %-40s " "$name"
        $CXX $CXXFLAGS $INCLUDE -o /tmp/note-cpp-ex "$ex" && echo "OK" || { echo "FAIL"; exit 1; }
    done

    # Verify embedded docs (first compiler only)
    if [ "${EMBEDME_DONE:-}" != "1" ]; then
        READMES=$(find "$ROOT/examples" -name 'README.md' 2>/dev/null)
        echo
        "$ROOT/tools/verify-docs.sh"
        export EMBEDME_DONE=1
    fi

    # Run coverage check (first compiler only) to catch regressions locally.
    if [ "${COVERAGE_DONE:-}" != "1" ]; then
        if command -v lcov >/dev/null 2>&1; then
            local lcov_major
            lcov_major=$(lcov --version 2>&1 | grep -oE '[0-9]+' | head -1)
            if [ "${lcov_major:-0}" -ge 2 ] 2>/dev/null; then
                echo
                run_coverage
                export COVERAGE_DONE=1
            fi
        fi
    fi

    # Final stage timing
    ci_stage "Done"
    printf "\nAll checks passed for %s in %ds.\n\n" "$CXX" $(( $(date +%s) - _ci_run_start ))
}

discover_compilers() {
    local compilers=()

    # Always include the default system compiler
    compilers+=("c++:-std=c++2b")

    # Look for GCC versions (Homebrew on macOS, system on Linux)
    for gxx in /usr/local/bin/g++-* /opt/homebrew/bin/g++-* /usr/bin/g++-12 /usr/bin/g++-13 /usr/bin/g++-14; do
        if [ -x "$gxx" ]; then
            local ver
            ver=$("$gxx" -dumpversion 2>/dev/null | cut -d. -f1)
            if [ "${ver:-0}" -ge 13 ] 2>/dev/null; then
                compilers+=("$gxx:-std=c++23")
            elif [ "${ver:-0}" -ge 12 ] 2>/dev/null; then
                compilers+=("$gxx:-std=c++20")
            fi
        fi
    done

    # Look for Clang versions with libc++ (Linux CI style)
    for clangxx in /usr/bin/clang++-17 /usr/bin/clang++-18 /usr/bin/clang++-19; do
        if [ -x "$clangxx" ]; then
            local cver
            cver=$("$clangxx" --version 2>/dev/null | grep -oE '[0-9]+' | head -1)
            if [ "${cver:-0}" -ge 18 ] 2>/dev/null; then
                compilers+=("$clangxx:-std=c++23 -stdlib=libc++")
            else
                compilers+=("$clangxx:-std=c++20 -stdlib=libc++")
            fi
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

# ── Coverage thresholds ─────────────────────────────────────────────────────
# Minimum acceptable coverage percentages. These match our current baselines
# (98.5% / 99.9% / 98.9%) with a small margin so that minor fluctuations
# from adding new code don't break the build before tests catch up.
MIN_LINE_COV=90
MIN_FUNC_COV=90
MIN_BRANCH_COV=85

check_coverage_thresholds() {
    local lcov_file="$1"
    local summary
    summary=$(lcov --summary "$lcov_file" --rc branch_coverage=1 2>&1)

    # Parse "  lines.......: 98.5% (5127 of 5206 lines)" etc.
    local lines funcs branches
    lines=$(echo "$summary" | awk '/lines/{gsub(/%/,""); print $2}')
    funcs=$(echo "$summary" | awk '/functions/{gsub(/%/,""); print $2}')
    branches=$(echo "$summary" | awk '/branches/{gsub(/%/,""); print $2}')

    echo
    local FAILED=0
    printf "  %-20s %6s%%  (minimum: %s%%)\n" "Lines:" "$lines" "$MIN_LINE_COV"
    printf "  %-20s %6s%%  (minimum: %s%%)\n" "Functions:" "$funcs" "$MIN_FUNC_COV"
    printf "  %-20s %6s%%  (minimum: %s%%)\n" "Branches:" "$branches" "$MIN_BRANCH_COV"
    echo

    # awk comparison handles decimals (bash can't do floating-point)
    if echo "$lines $MIN_LINE_COV" | awk '{exit !($1 < $2)}'; then
        echo "FAIL: line coverage ${lines}% < ${MIN_LINE_COV}%"
        FAILED=1
    fi
    if echo "$funcs $MIN_FUNC_COV" | awk '{exit !($1 < $2)}'; then
        echo "FAIL: function coverage ${funcs}% < ${MIN_FUNC_COV}%"
        FAILED=1
    fi
    if echo "$branches $MIN_BRANCH_COV" | awk '{exit !($1 < $2)}'; then
        echo "FAIL: branch coverage ${branches}% < ${MIN_BRANCH_COV}%"
        FAILED=1
    fi
    if [ "$FAILED" -ne 0 ]; then
        echo
        echo "Coverage below minimum thresholds. See coverage/html/index.html for details."
        exit 1
    fi
    echo "  All coverage thresholds met."
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
        "$ROOT/tests/test_endpoint_coverage.cpp" \
        "$ROOT/tests/test_voltage_variable.cpp" \
        "$ROOT/tests/test_flag_set.cpp" \
        "$ROOT/tests/test_json_sax.cpp" \
        "$ROOT/tests/test_target.cpp" \
        "$ROOT/tests/test_make_api.cpp" \
        "$ROOT/tests/test_units.cpp"
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

    check_coverage_thresholds "$OUT_DIR/coverage.lcov"

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

    # lcov 2.x required: version 1.x cannot parse GCC 13+ .gcno format files,
    # causing "Overlong record" errors or silently empty reports.
    # Install: 'brew install lcov' (macOS) or 'apt-get install lcov' (Ubuntu 24.04+).
    if ! command -v lcov >/dev/null 2>&1; then
        echo "ERROR: lcov not found. Install: 'brew install lcov' (macOS) or 'apt-get install lcov' (Ubuntu)."
        exit 1
    fi
    local lcov_major
    lcov_major=$(lcov --version 2>&1 | grep -oE '[0-9]+' | head -1)
    if [ "${lcov_major:-0}" -lt 2 ] 2>/dev/null; then
        echo "ERROR: lcov 2.x or newer is required (found: $(lcov --version 2>&1 | head -1))."
        echo "       Upgrade: 'brew upgrade lcov' (macOS) or 'apt-get install lcov' (Ubuntu 24.04+)."
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
        test_voltage_variable test_flag_set test_json_sax
        test_channel test_state_store
        test_target test_make_api test_units
        test_connection test_sync test_templates test_attention test_setup
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

    check_coverage_thresholds "$OUT_DIR/coverage.lcov"

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

run_docs() {
    echo "=== Doxygen documentation ==="
    if ! command -v doxygen >/dev/null 2>&1; then
        echo "ERROR: doxygen not found. Install: 'brew install doxygen' (macOS) or 'apt-get install doxygen' (Ubuntu)."
        exit 1
    fi
    echo "  Doxygen $(doxygen --version)"
    doxygen "$ROOT/Doxyfile"
    echo

    # Validate internal links in generated HTML
    echo "=== Link validation ==="
    local html_dir="$ROOT/docs/html"
    local broken_file
    broken_file=$(mktemp)
    echo "0" > "$broken_file"
    local checked_file
    checked_file=$(mktemp)
    echo "0" > "$checked_file"

    find "$html_dir" -name '*.html' -type f | while IFS= read -r file; do
        dir=$(dirname "$file")
        grep -oE 'href="[^"#]+"' "$file" 2>/dev/null | sed 's/href="//;s/"$//' | while IFS= read -r href; do
            case "$href" in
                http://*|https://*|javascript:*) continue ;;
            esac
            target="${href%%#*}"
            [ -z "$target" ] && continue
            echo $(( $(cat "$checked_file") + 1 )) > "$checked_file"
            if [ ! -f "$dir/$target" ]; then
                echo "  BROKEN: $(basename "$file") -> $href"
                echo $(( $(cat "$broken_file") + 1 )) > "$broken_file"
            fi
        done
    done
    local broken checked
    broken=$(cat "$broken_file")
    checked=$(cat "$checked_file")
    rm -f "$broken_file" "$checked_file"
    if [ "$broken" -gt 0 ]; then
        echo "  ERROR: $broken broken link(s) found out of $checked"
        exit 1
    fi
    echo "  All links valid ($checked checked)"
    echo

    echo "  Documentation: ${ROOT}/docs/html/index.html"
    echo
}

run_quick() {
    local CXX="${1:-${CXX:-c++}}"
    local CXXFLAGS="${2:-${CXXFLAGS:--std=c++2b}} -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror"
    local INCLUDE="-I $ROOT/include"

    echo "════════════════════════════════════════════════════════════════"
    echo "Quick check: codegen + unit tests"
    echo "Compiler: $($CXX --version | head -1)"
    echo "════════════════════════════════════════════════════════════════"
    echo

    # Code generation
    PYTHON=python3
    if [ -f "$ROOT/.venv/bin/python3" ]; then
        PYTHON="$ROOT/.venv/bin/python3"
    fi
    if command -v "$PYTHON" >/dev/null 2>&1; then
        echo "=== Code generation ==="
        "$PYTHON" "$ROOT/tools/codegen/generate.py" "$ROOT/notecard-api.openapi.json" \
            -o "$ROOT/include/note/api" \
            --api "$ROOT/include/note/api.hpp" \
            --test-dir "$ROOT/tests"
        echo
    fi

    # Build and run unit tests
    ci_stage "Unit tests"
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
        "$ROOT/tests/test_endpoint_coverage.cpp" \
        "$ROOT/tests/test_voltage_variable.cpp" \
        "$ROOT/tests/test_flag_set.cpp" \
        "$ROOT/tests/test_json_sax.cpp" \
        "$ROOT/tests/test_channel.cpp" \
        "$ROOT/tests/test_state_store.cpp" \
        "$ROOT/tests/test_target.cpp" \
        "$ROOT/tests/test_make_api.cpp" \
        "$ROOT/tests/test_units.cpp" \
        "$ROOT/tests/test_connection.cpp" \
        "$ROOT/tests/test_sync.cpp" \
        "$ROOT/tests/test_templates.cpp" \
        "$ROOT/tests/test_attention.cpp" \
        "$ROOT/tests/test_setup.cpp"
    /tmp/note-cpp-tests
    echo "  tests: OK"
    echo
    echo "Quick check passed."
    echo
}

run_integrations() {
    echo "=== JSON backend integration tests ==="
    local CMAKE_POLICY="-DCMAKE_POLICY_VERSION_MINIMUM=3.5"

    for backend in cjson nlohmann buffer; do
        local src="$ROOT/tests/integration/$backend"
        local build="/tmp/note-cpp-integration-$backend"
        echo
        echo "--- $backend backend ---"
        cmake -B "$build" "$src" $CMAKE_POLICY -DCMAKE_CXX_STANDARD=20 2>&1 | tail -3
        cmake --build "$build" 2>&1
        ctest --test-dir "$build" --output-on-failure
    done

    echo
    echo "All integration tests passed."
}

case "${1:-}" in
    --full)
        run_ci "${CXX:-c++}" "${CXXFLAGS:--std=c++2b}"
        ;;
    --coverage)
        run_coverage
        ;;
    --docs)
        run_docs
        ;;
    --integrations)
        run_integrations
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
        run_quick "${CXX:-c++}" "${CXXFLAGS:--std=c++2b}"
        ;;
esac
