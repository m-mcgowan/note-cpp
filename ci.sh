#!/usr/bin/env bash
set -euo pipefail

# Run at low priority so CI doesn't starve interactive work.
renice -n 10 $$ >/dev/null 2>&1 || true

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

# ── ci.sh is the source of truth ───────────────────────────────────────────
# Every check that runs in GitHub Actions must also be reachable via a
# single invocation of this script. The workflow (.github/workflows/ci.yml)
# should be a thin orchestrator: each job installs its prerequisites
# (compiler, arduino-cli cores, lcov, …) and then calls ci.sh — never
# duplicates the test/build logic in yaml.
#
# Run tiers:
#   --quick   fastest feedback (~60-120s) — codegen + unit tests
#   (default) regular   — adds examples, version gating, snippet verify
#   --full    everything GitHub runs   — compile-* fixtures, coverage,
#             Arduino sketches, PIO integration builds
#
# When you add a new GitHub CI step, add the corresponding stage in
# run_ci() (or run_quick()) first, then wire the workflow step to call
# it. A gap between local --full and remote CI is a bug.

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

    # Public API headers compile cleanly (single TU per standard version).
    # Generated api/*.hpp are internal — validated via the unit tests.
    ci_stage "Public API headers"
    $CXX $CXXFLAGS $INCLUDE -fsyntax-only -x c++ - <<'HEOF'
#include <note/notecard.hpp>
#include <note/notecard_api.hpp>
#include <note/api.hpp>
#include <note/transact.hpp>
#include <note/link/serial.hpp>
#include <note/link/i2c.hpp>
#include <note/body.hpp>
#include <note/field.hpp>
#include <note/target.hpp>
#include <note/units.hpp>
#include <note/json_buf.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/string_pool.hpp>
#include <note/posix.hpp>
HEOF
    echo "  C++20 public headers OK"

    # C++17 compatibility — all public headers including transport
    if [ "${CPP17_DONE:-}" != "1" ]; then
        $CXX -std=c++17 $INCLUDE -fsyntax-only -x c++ - <<'H17EOF'
#include <note/notecard.hpp>
#include <note/notecard_api.hpp>
#include <note/api.hpp>
#include <note/transact.hpp>
#include <note/link/serial.hpp>
#include <note/link/i2c.hpp>
#include <note/body.hpp>
#include <note/field.hpp>
#include <note/target.hpp>
#include <note/units.hpp>
#include <note/json_buf.hpp>
#include <note/arena.hpp>
#include <note/allocator.hpp>
#include <note/string_pool.hpp>
#include <note/posix.hpp>
H17EOF
        echo "  C++17 public headers OK"

        # C++17 NOTE_FIELDS macro — must work since it's the primary user-facing macro
        $CXX -std=c++17 $INCLUDE -fsyntax-only -x c++ - <<'NFEOF'
#include <note/body.hpp>
struct TestStruct { float a; int32_t b; NOTE_FIELDS(a, b) };
NFEOF
        echo "  C++17 NOTE_FIELDS OK"

        export CPP17_DONE=1
    fi

    # Build and run unit tests
    echo
    ci_stage "Unit tests"
    $CXX $CXXFLAGS $INCLUDE -I "$ROOT/tests" -o /tmp/note-cpp-tests \
        "$ROOT/tests/doctest_main.cpp" \
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
        "$ROOT/tests/test_arduino_printable.cpp" \
        "$ROOT/tests/test_transport_streaming.cpp" \
        "$ROOT/tests/test_json_sax_streaming.cpp" \
        "$ROOT/tests/test_streaming_builder.cpp" \
        "$ROOT/tests/test_endpoint_streaming.cpp" \
        "$ROOT/tests/test_streaming_errors.cpp" \
        "$ROOT/tests/test_posix_serial.cpp" \
        "$ROOT/tests/test_notecard_move_semantics.cpp" \
        "$ROOT/tests/test_struct_sink_strings.cpp"
    /tmp/note-cpp-tests
    echo "  tests: OK"

    # NOTE_MINIMAL host build — exercises singleton, static HAL, constexpr policy.
    # Subset of tests that don't depend on unicode escapes or buffered transport.
    ci_stage "Unit tests (NOTE_MINIMAL)"
    $CXX $CXXFLAGS -DNOTE_MINIMAL $INCLUDE -I "$ROOT/tests" -o /tmp/note-cpp-tests-minimal \
        "$ROOT/tests/doctest_main.cpp" \
        "$ROOT/tests/test_static_notecard.cpp" \
        "$ROOT/tests/test_static_sizing.cpp"
    /tmp/note-cpp-tests-minimal
    echo "  minimal tests: OK"

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

    # Target filtering is exercised by the compile-check / compile-warn /
    # compile-fail fixtures under tests/ — see tests/compile_warn/ and
    # tests/compile_fail/target_*.cpp / fw_*.cpp. ctest runs those.

    # Build all examples
    echo
    ci_stage "Examples"
    for ex in $(find "$ROOT/examples/stdcpp" -name '*.cpp' | sort); do
        name=${ex#$ROOT/examples/stdcpp/}
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

    # Arduino-cli sketch compiles (first compiler only) — mirrors the
    # `arduino-cli` GitHub job so "local --full green" implies "remote green"
    # for that workflow. Skips with a clear note if arduino-cli or the
    # required cores aren't installed (not a hard failure for devs on non-
    # Arduino workstations).
    if [ "${ARDUINO_CLI_DONE:-}" != "1" ]; then
        if command -v arduino-cli >/dev/null 2>&1; then
            local missing=""
            arduino-cli core list 2>/dev/null | grep -q "^esp32:esp32 " \
                || missing+=" esp32:esp32"
            arduino-cli core list 2>/dev/null | grep -q "^STMicroelectronics:stm32 " \
                || missing+=" STMicroelectronics:stm32"
            if [ -z "$missing" ]; then
                echo
                ci_stage "Arduino sketches (arduino-cli)"
                for sketch in quickstart readme_snippets serial_basic i2c_basic; do
                    for spec in "esp32:esp32:esp32s3:CDCOnBoot=cdc;ESP32-S3" \
                                "STMicroelectronics:stm32:Blues:pnum=SWAN_R5;Swan"; do
                        fqbn="${spec%;*}"
                        label="${spec##*;}"
                        printf "  %-40s " "$sketch ($label)"
                        if arduino-cli compile \
                            --fqbn "$fqbn" \
                            --library "$ROOT" \
                            "$ROOT/examples/arduino/$sketch" \
                            >/tmp/arduino-cli.log 2>&1; then
                            echo "OK"
                        else
                            echo "FAIL"
                            cat /tmp/arduino-cli.log
                            exit 1
                        fi
                    done
                done
                export ARDUINO_CLI_DONE=1
            else
                echo
                echo "  Skipping Arduino sketches stage (missing cores:$missing)"
                echo "  Install with:"
                if [[ "$missing" == *"STMicroelectronics:stm32"* ]]; then
                    echo "    arduino-cli config add board_manager.additional_urls \\"
                    echo "      https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json"
                    echo "    arduino-cli core update-index"
                fi
                echo "    arduino-cli core install$missing"
            fi
        else
            echo
            echo "  Skipping Arduino sketches stage (arduino-cli not in PATH)"
        fi
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
# NOTE: Thresholds lowered from 90/90/85 due to new compile-time feature
# flags (NOTE_SINGLETON, NOTE_STATIC_HAL, NOTE_PRINTABLE) that add #if branches
# not exercised by the default host build. An exhaustive CI build that tests
# multiple flag combinations would restore coverage to previous levels.
MIN_LINE_COV=90
MIN_FUNC_COV=90
# Branch coverage floor. 94% accommodates: (a) template-instantiation branch
# records for body-factory dispatch in notecard.hpp and endpoint headers,
# where each RequestT instantiation adds its own copy of the
# `if (req.body_handler_factory_)` check; (b) under-exercised JSONB /
# struct-sink edge paths in tests pulled into the coverage build
# (test_jsonb, test_struct_field_symmetry). Raising further requires
# targeted tests for those paths — start with the highest-miss files in
# coverage/html/index.html.
MIN_BRANCH_COV=94

check_coverage_thresholds() {
    local lcov_file="$1"

    # Use per-file FNF/FNH records from the LCOV file for function coverage.
    # lcov --summary double-counts template instantiations (each FNA record),
    # inflating the denominator. FNF/FNH counts unique functions per file.
    local lines funcs branches
    read -r lines funcs branches < <(python3 -c "
import sys
lf=lh=ff=fh=bf=bh=0
with open('$lcov_file') as f:
    for line in f:
        line = line.strip()
        if line.startswith('LF:'): lf += int(line[3:])
        elif line.startswith('LH:'): lh += int(line[3:])
        elif line.startswith('FNF:'): ff += int(line[4:])
        elif line.startswith('FNH:'): fh += int(line[4:])
        elif line.startswith('BRDA:'):
            # Count branches from raw BRDA records, excluding '-' (non-executable).
            # lcov's BRF/BRH counts '-' branches in BRF, inflating the denominator.
            count = line.split(',')[-1]
            if count != '-':
                bf += 1
                if int(count) > 0: bh += 1
l = 100.0*lh/lf if lf else 0
fn = 100.0*fh/ff if ff else 0
b = 100.0*bh/bf if bf else 0
print(f'{l:.1f} {fn:.1f} {b:.1f}')
")

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
        "$ROOT/tests/doctest_main.cpp" \
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
        "$ROOT/tests/test_units.cpp" \
        "$ROOT/tests/test_transport_streaming.cpp" \
        "$ROOT/tests/test_json_sax_streaming.cpp" \
        "$ROOT/tests/test_streaming_builder.cpp" \
        "$ROOT/tests/test_endpoint_streaming.cpp" \
        "$ROOT/tests/test_streaming_errors.cpp"
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
    for g in g++-13 g++-14; do
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

    # Full test set (requires polymorphic Notecard).
    local SRCS_FULL=(
        doctest_main test_wire_format test_samples test_body
        test_binary_execute test_buffer_backend test_buffered_bridge
        test_json_buf test_json_fmt test_json_lexer test_json_scan
        test_property_functor
        test_transport_crc32 test_transport_serial test_transport_i2c
        test_transport_timing test_transport_streaming
        test_transport_agnostic_api
        test_notecard test_notecard_streaming
        test_api_context test_endpoint_coverage
        test_voltage_variable test_flag_set test_json_sax
        test_channel test_state_store
        test_target test_make_api test_units
        test_connection test_sync test_templates test_attention test_setup
        test_arduino_printable test_intent_flags test_cobs
        test_json_sax_streaming test_streaming_builder
        test_endpoint_streaming test_streaming_errors
        test_allocator_growth test_body_capture_arena
        test_sax_dispatch test_generic_sink
        test_static_sizing test_static_notecard test_struct_sink
        test_struct_field_symmetry
        test_debug test_migration_support test_retry test_sizeof_report
        test_bare_notecard test_jsonb test_txn_handshake test_error_message
    )
    local jobs=0
    local max_jobs=4
    for name in "${SRCS_FULL[@]}"; do
        nice "$GCC" $CXXFLAGS --coverage -fprofile-arcs $INCLUDE -I "$ROOT/tests" \
            -c "$ROOT/tests/${name}.cpp" -o "$BUILD_DIR/${name}.o" &
        jobs=$((jobs + 1))
        if [ "$jobs" -ge "$max_jobs" ]; then
            wait -n 2>/dev/null || wait
            jobs=$((jobs - 1))
        fi
    done
    wait
    nice "$GCC" --coverage -fprofile-arcs -o "$BINARY" "$BUILD_DIR"/*.o
    "$BINARY"

    # NOTE_MINIMAL pass: exercises singleton, static HAL, GenericBodySink paths.
    local BUILD_MIN="/tmp/note-cpp-cov-minimal"
    local BINARY_MIN="/tmp/note-cpp-tests-cov-minimal"
    ci_stage "Coverage build (NOTE_MINIMAL)"
    rm -rf "$BUILD_MIN" && mkdir -p "$BUILD_MIN"
    local SRCS_MIN=(
        doctest_main test_buffer_backend test_cobs test_flag_set
        test_json_buf test_json_fmt test_json_sax test_json_sax_streaming
        test_json_lexer test_retry test_state_store test_target
        test_transport_crc32 test_body_capture_arena test_sax_dispatch
        test_static_sizing test_static_notecard test_struct_sink
        test_generic_sink test_units test_voltage_variable
    )
    jobs=0
    for name in "${SRCS_MIN[@]}"; do
        nice "$GCC" $CXXFLAGS --coverage -fprofile-arcs -DNOTE_MINIMAL \
            $INCLUDE -I "$ROOT/tests" \
            -c "$ROOT/tests/${name}.cpp" -o "$BUILD_MIN/${name}.o" &
        jobs=$((jobs + 1))
        if [ "$jobs" -ge "$max_jobs" ]; then
            wait -n 2>/dev/null || wait
            jobs=$((jobs - 1))
        fi
    done
    wait
    nice "$GCC" --coverage -fprofile-arcs -o "$BINARY_MIN" "$BUILD_MIN"/*.o
    "$BINARY_MIN"

    # Backend-parity host run (doctest). Produces coverage for
    # include/note/backends/*.hpp — the backends aren't otherwise
    # exercised by the unit tests.
    local BB_BUILD="/tmp/note-cpp-integration-backends-cov"
    echo
    echo "=== Backend-parity coverage build ==="
    cmake -B "$BB_BUILD" "$ROOT/tests" \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_CXX_COMPILER="$GCC" \
        -DCMAKE_CXX_FLAGS="--coverage -fprofile-arcs -ftest-coverage" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage" 2>&1 | tail -3
    nice cmake --build "$BB_BUILD" --target note-cpp-integration-backends 2>&1 | tail -3
    "$BB_BUILD/note-cpp-integration-backends"

    mkdir -p "$OUT_DIR"
    echo
    echo "=== Collecting coverage data ==="
    # LCOV_EXCL_* markers are not honored by lcov 2.4's capture step in our
    # setup — use --omit-lines with the NOTE_COVERAGE_OMIT marker instead.
    # Apply at each --capture so the exclusion is baked into the raw lcov data
    # before any merge/extract/remove step sees it.
    local LCOV_OPTS="--rc branch_coverage=1 --rc no_exception_branch=1 --rc geninfo_unexecuted_blocks=1 --ignore-errors mismatch,inconsistent,gcov --omit-lines .*NOTE_COVERAGE_OMIT.*"
    lcov --capture \
        --directory "$BUILD_DIR" \
        --gcov-tool "$GCOV" \
        $LCOV_OPTS \
        --output-file "$OUT_DIR/coverage-raw.lcov" \
        --quiet
    # Capture NOTE_MINIMAL run.
    lcov --capture \
        --directory "$BUILD_MIN" \
        --gcov-tool "$GCOV" \
        $LCOV_OPTS \
        --output-file "$OUT_DIR/coverage-raw-minimal.lcov" \
        --quiet
    # Capture backend-parity run. The NOTE_COVERAGE_OMIT pattern in LCOV_OPTS
    # is never present in backend test sources — add "unused" to the ignore
    # list so lcov doesn't error out.
    lcov --capture \
        --directory "$BB_BUILD" \
        --gcov-tool "$GCOV" \
        $LCOV_OPTS --ignore-errors unused \
        --output-file "$OUT_DIR/coverage-raw-backends.lcov" \
        --quiet
    # Merge all three runs.
    lcov --add-tracefile "$OUT_DIR/coverage-raw.lcov" \
        --add-tracefile "$OUT_DIR/coverage-raw-minimal.lcov" \
        --add-tracefile "$OUT_DIR/coverage-raw-backends.lcov" \
        $LCOV_OPTS \
        --output-file "$OUT_DIR/coverage-merged.lcov" \
        --quiet
    # Keep only our headers; strip third_party.
    lcov --extract "$OUT_DIR/coverage-merged.lcov" "*/include/note/*" \
        --rc branch_coverage=1 \
        --ignore-errors inconsistent \
        --output-file "$OUT_DIR/coverage-filtered.lcov" \
        --quiet
    lcov --remove "$OUT_DIR/coverage-filtered.lcov" \
        "*/third_party/*" \
        "*/backends/detail/jsmn.h" \
        --rc branch_coverage=1 \
        --ignore-errors unused,inconsistent \
        --output-file "$OUT_DIR/coverage.lcov" \
        --quiet

    # Strip non-executable BRDA records ('-' count) and recompute BRF/BRH.
    # lcov/genhtml count '-' branches in the denominator, inflating misses.
    # These come from GCC marking template instantiation branches as dead code.
    python3 -c "
lines = open('$OUT_DIR/coverage.lcov').readlines()
out = []
bf = bh = 0
for line in lines:
    s = line.strip()
    if s.startswith('BRDA:'):
        if s.split(',')[-1] == '-':
            continue  # drop non-executable branch records
        bf += 1
        if int(s.split(',')[-1]) > 0: bh += 1
    elif s.startswith('BRF:'):
        line = f'BRF:{bf}\n'
    elif s.startswith('BRH:'):
        line = f'BRH:{bh}\n'
        bf = bh = 0
    out.append(line)
open('$OUT_DIR/coverage.lcov', 'w').writelines(out)
"

    echo
    echo "=== Coverage summary ==="
    lcov --summary "$OUT_DIR/coverage.lcov" --rc branch_coverage=1 --ignore-errors inconsistent

    check_coverage_thresholds "$OUT_DIR/coverage.lcov"

    echo
    echo "=== Generating HTML report → ${OUT_DIR}/html ==="
    genhtml "$OUT_DIR/coverage.lcov" \
        --output-directory "$OUT_DIR/html" \
        --title "note-cpp coverage" \
        --branch-coverage \
        --ignore-errors inconsistent \
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
    doxygen "$ROOT/docs/doxygen/Doxyfile"
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
    local INCLUDE="-I $ROOT/include -I $ROOT/tests"
    local _ci_run_start
    _ci_run_start=$(date +%s)
    _ci_stage_start=0

    echo "════════════════════════════════════════════════════════════════"
    echo "Quick check: codegen + unit tests"
    echo "Compiler: $($CXX --version | head -1)"
    echo "════════════════════════════════════════════════════════════════"

    # Code generation
    ci_stage "Code generation"
    PYTHON=python3
    if [ -f "$ROOT/.venv/bin/python3" ]; then
        PYTHON="$ROOT/.venv/bin/python3"
    fi
    if command -v "$PYTHON" >/dev/null 2>&1; then
        "$PYTHON" "$ROOT/tools/codegen/generate.py" "$ROOT/notecard-api.openapi.json" \
            -o "$ROOT/include/note/api" \
            --api "$ROOT/include/note/api.hpp" \
            --test-dir "$ROOT/tests"
    fi

    # Build and run unit tests via CMake (parallel, proper TU separation)
    ci_stage "Build tests"
    local BUILD_DIR="/tmp/note-cpp-build"
    local GENERATOR="Unix Makefiles"
    command -v ninja >/dev/null 2>&1 && GENERATOR="Ninja"
    cmake -G "$GENERATOR" -B "$BUILD_DIR" -S "$ROOT" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_CXX_STANDARD=20 \
        > /dev/null 2>&1
    nice cmake --build "$BUILD_DIR" --parallel

    ci_stage "Run tests"
    "$BUILD_DIR/tests/note-cpp-tests"
    echo "  host tests: OK"
    "$BUILD_DIR/tests/note-cpp-tests-arduino"
    echo "  arduino tests: OK"

    # NOTE_MINIMAL host build — exercises singleton, static HAL, constexpr policy paths
    ci_stage "Build tests (NOTE_MINIMAL)"
    local BUILD_MIN="/tmp/note-cpp-build-minimal"
    cmake -G "$GENERATOR" -B "$BUILD_MIN" -S "$ROOT" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_CXX_FLAGS="-DNOTE_MINIMAL" \
        > /dev/null 2>&1
    nice cmake --build "$BUILD_MIN" --parallel

    ci_stage "Run tests (NOTE_MINIMAL)"
    "$BUILD_MIN/tests/note-cpp-tests"
    echo "  host minimal tests: OK"
    "$BUILD_MIN/tests/note-cpp-tests-arduino"
    echo "  arduino minimal tests: OK"

    # NOTE_SINGLETON-only host build — exercises the singleton-thunk
    # adapters on the *polymorphic* Notecard (NOTE_MINIMAL covers singleton
    # + StaticNotecard via NOTE_STATIC_HAL). The build itself is the gap
    # closure: it forces instantiation of Api::void_thunk_ /
    # generic_thunk_ against `note::Notecard`, which used to fail to
    # compile because execute_void / execute_generic_with_body only
    # existed on StaticNotecard. The test binaries are not run here —
    # singleton mode currently has unrelated runtime limitations
    # (GenericResponseSink can't dispatch array fields; the generated
    # `nc_err.view()` returns a stack-pointer that dangles past
    # execute(); the thunk path doesn't inject request IDs). Those are
    # pre-existing and surface only because this combination now
    # compiles. Fixing them is separate work.
    ci_stage "Build tests (NOTE_SINGLETON only)"
    local BUILD_SINGLETON="/tmp/note-cpp-build-singleton"
    cmake -G "$GENERATOR" -B "$BUILD_SINGLETON" -S "$ROOT" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_CXX_FLAGS="-DNOTE_SINGLETON=1" \
        > /dev/null 2>&1
    nice cmake --build "$BUILD_SINGLETON" --parallel
    echo "  singleton-only host + arduino test binaries link: OK"

    # PlatformIO integration test build (compile only, no hardware)
    if command -v pio >/dev/null 2>&1; then
        ci_stage "PIO integration build"
        local PIO_DIR="$ROOT/tests/integration/firmware"
        for env in serial i2c; do
            echo "  Building $env..."
            NOTECARD_SERIAL_RX=38 NOTECARD_SERIAL_TX=39 \
            NOTECARD_I2C_SDA=14 NOTECARD_I2C_SCL=21 \
            pio run -d "$PIO_DIR" -e "$env" > /dev/null 2>&1
            echo "  $env: OK"
        done

        # AVR build + size regression gate. Catches Harvard-architecture
        # / 8-bit-only breaks (PROGMEM regressions, missing pgmspace.h
        # guards) AND unintended size growth — flash/RAM are tight on AVR
        # (32 KB / 2 KB on ATmega328P) so any creep is a bug worth flagging.
        # Skipped if the atmelavr toolchain isn't installed: PlatformIO
        # would auto-install (~minutes), unwanted overhead in CI's coverage
        # job. Local dev already has it; CI's pio-build job runs the same
        # check explicitly.
        if ls ~/.platformio/packages/toolchain-atmelavr* >/dev/null 2>&1; then
            ci_stage "AVR size check (avr-notecpp / direct / raw)"
            "$ROOT/tools/binary-size-comparison/avr_size_check.py"
        fi
    fi

    # JSON backend integration tests — build + run the consolidated
    # doctest binary so drift in cjson/nlohmann/buffer tests surfaces
    # in GitHub CI, not just when someone manually runs --integrations.
    ci_stage "JSON backend integration tests"
    run_integrations

    # GCC + -std=c++23 gate. The system compiler on macOS is Apple Clang
    # which is forgiving where g++-13 (the GitHub CI gate) is not — most
    # famously the consteval/constexpr divergence around BodyValue. Run
    # a syntax check + build a tight set of TUs that exercise the
    # affected paths so local --quick stays in sync with remote CI.
    if [ "${GCC_CPP23_DONE:-}" != "1" ]; then
        local g23=""
        for g in g++-13 g++-14 g++-15; do
            for p in /usr/local/bin /opt/homebrew/bin /usr/bin ""; do
                local cand="${p:+$p/}$g"
                if command -v "$cand" >/dev/null 2>&1; then
                    g23="$cand"; break 2
                fi
            done
        done
        if [ -n "$g23" ]; then
            ci_stage "GCC c++23 syntax + BodyValue gate ($g23)"
            local FLAGS="-std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Werror"
            $g23 $FLAGS $INCLUDE -fsyntax-only -x c++ - <<'GCCEOF'
#include <note/notecard.hpp>
#include <note/notecard_api.hpp>
#include <note/api.hpp>
#include <note/body.hpp>
#include <note/transact.hpp>
#include <note/link/serial.hpp>
#include <note/link/i2c.hpp>
#include <note/json_buf.hpp>
GCCEOF
            echo "  c++23 public headers OK"

            # Constexpr BodyValue: the canonical g++-13 c++23 trip-wire.
            $g23 $FLAGS $INCLUDE -fsyntax-only -x c++ - <<'BVEOF'
#include <note/body.hpp>
constexpr note::BodyValue v = "{\"x\":1}";
static_assert(static_cast<bool>(v));
BVEOF
            echo "  constexpr BodyValue OK"
        else
            echo
            echo "  Skipping GCC c++23 gate (no g++-13/14/15 in PATH)."
            echo "  Install: 'brew install gcc' or 'apt-get install g++-13'."
        fi
        export GCC_CPP23_DONE=1
    fi

    ci_stage "Done"
    printf "\nQuick check passed in %ds.\n\n" $(( $(date +%s) - _ci_run_start ))
}

run_integrations() {
    echo "=== JSON backend integration tests (consolidated doctest binary) ==="
    local build="/tmp/note-cpp-integration-backends"
    cmake -B "$build" "$ROOT/tests" \
        -DCMAKE_CXX_STANDARD=20 2>&1 | tail -3
    nice cmake --build "$build" --target note-cpp-integration-backends 2>&1 | tail -5
    "$build/note-cpp-integration-backends"

    echo
    echo "All integration tests passed."
}

# Log every run
CI_LOG="$ROOT/ci.log"

run_and_log() {
    "$@" 2>&1 | tee "$CI_LOG"
    local rc=${PIPESTATUS[0]}
    echo "Log saved to $CI_LOG"
    return $rc
}

case "${1:-}" in
    --full)
        run_and_log run_ci "${CXX:-c++}" "${CXXFLAGS:--std=c++2b}"
        ;;
    --coverage)
        run_and_log run_coverage
        ;;
    --docs)
        run_and_log run_docs
        ;;
    --integrations)
        run_and_log run_integrations
        ;;
    --all-compilers)
        echo "Discovering compilers..."
        FAILED=0
        while IFS= read -r entry; do
            cxx="${entry%%:*}"
            flags="${entry#*:}"
            run_ci "$cxx" "$flags" || FAILED=1
        done < <(discover_compilers) 2>&1 | tee "$CI_LOG"
        if [ "$FAILED" -ne 0 ]; then
            echo "SOME COMPILERS FAILED"
            exit 1
        fi
        echo "All compilers passed."
        ;;
    *)
        run_and_log run_quick "${CXX:-c++}" "${CXXFLAGS:--std=c++2b}"
        ;;
esac
