#!/usr/bin/env bash
# CI script for the integration test firmware.
#
# Verifies that each PlatformIO environment builds correctly and that the
# no-interface guard fires a compilation error.
#
# Usage:
#   ./ci.sh                # build-only (CI, no hardware)
#   ./ci.sh --test         # build + upload + run on hardware
#   ./ci.sh --coverage     # device coverage flow (see below)
#
# Hardware test requires:
#   source boards.sh 1.9   # or your board revision
#   ./ci.sh --test --upload-port /dev/cu.usbmodem...
#
# --coverage mode: runs the integration suite under instrumentation,
# captures `COV:` records to `.pio/build/<env>/<partition>.cov` via
# pio-cov's `CovReceiver` (auto-attached when pio-cov is pip-installed
# in PIO's penv via the `embedded_test_runner.receivers` entry-point
# group), decodes each `.cov` to lcov `.info`, and merges into a
# combined device-coverage report under ./coverage/. Partitions:
#
#   test_fixtures  — gcov backend (small enough to fit DRAM)
#   test_units_a   — trace-pc backend (gcov backend DRAM-overflows)
#   test_units_b   — trace-pc backend (gcov backend DRAM-overflows)
#   test_units_c   — trace-pc backend (gcov backend DRAM-overflows)
#
# All four partitions use the trace-pc envs' loopTask stack of 24 KB —
# the codegen-emitted "Api::<group> resource group" tests pin ~150 B of
# stack per distinct request type, and the "card" group's 130-call
# block pushes the live frame to ~19.5 KB at any optimisation level
# (sequential temporaries of distinct types don't get slot-merged by
# GCC). Default Arduino-ESP32 8 KB and the previous 16 KB both blew the
# canary; 24 KB has comfortable headroom.
#
# Decoding requires the `pio_cov` Python module. Preferred:
# `pip install pio-cov` in PIO's penv. Otherwise the script falls back
# to PYTHONPATH against `${PIO_COV_ROOT:-$HOME/e/pio-cov}`.
#
# Any extra arguments after --test or --coverage are forwarded to
# `pio test` (e.g. `--upload-port`, `--test-port`, `-v`).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RUN_TESTS=0
COVERAGE_MODE=0
DECODE=1
PIO_EXTRA_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --test)       RUN_TESTS=1 ;;
        --coverage)   COVERAGE_MODE=1; RUN_TESTS=1 ;;
        --no-decode)  DECODE=0 ;;
        *)            PIO_EXTRA_ARGS+=("$arg") ;;
    esac
done

passed=0
failed=0

# Build (or test) a PlatformIO environment.
#   $1 = env name
#   $2 = "expect_fail" if compilation should fail (optional)
run_env() {
    local env="$1"
    local expect_fail="${2:-}"

    if [ "$expect_fail" = "expect_fail" ]; then
        echo "=== $env (expect compile error) ==="
        if pio test -e "$env" --without-uploading --without-testing >/dev/null 2>&1; then
            echo "FAIL: $env should have failed to compile but succeeded"
            failed=$((failed + 1))
        else
            echo "OK: $env correctly failed to compile"
            passed=$((passed + 1))
        fi
    elif [ "$RUN_TESTS" = "1" ]; then
        echo "=== Testing: $env ==="
        if pio test -e "$env" "${PIO_EXTRA_ARGS[@]}"; then
            echo "OK: $env"
            passed=$((passed + 1))
        else
            echo "FAIL: $env"
            failed=$((failed + 1))
        fi
    else
        echo "=== Building: $env ==="
        if pio test -e "$env" --without-uploading --without-testing; then
            echo "OK: $env"
            passed=$((passed + 1))
        else
            echo "FAIL: $env"
            failed=$((failed + 1))
        fi
    fi

    echo
}

# Run a coverage partition: pio test under the right env, capture .cov
# via the CovReceiver, optionally decode via pio_cov.
#   $1 = backend ("gcov" or "tracepc")
#   $2 = partition name (e.g. "test_fixtures")
run_coverage_partition() {
    local backend="$1"
    local partition="$2"
    local env_name
    case "$backend" in
        gcov)    env_name="serial-coverage" ;;
        tracepc) env_name="serial-coverage-tracepc" ;;
        *)       echo "FAIL: unknown coverage backend: $backend"; failed=$((failed+1)); return ;;
    esac

    echo "=== $env_name:$partition ($backend) ==="
    if pio test -e "$env_name" -f "$partition" "${PIO_EXTRA_ARGS[@]}"; then
        echo "OK: $env_name:$partition"
        passed=$((passed+1))
    else
        echo "FAIL: $env_name:$partition"
        failed=$((failed+1))
        return
    fi

    COV_RUNS+=("$backend|$env_name|$partition")
}

# Decode all captured .cov files in COV_RUNS to per-partition .info,
# merge into ./coverage/device.info, and print a summary.
decode_and_merge_coverage() {
    [ "$DECODE" -ne 1 ] && { echo "Skipping decode (--no-decode)"; return; }
    [ "${#COV_RUNS[@]}" -eq 0 ] && { echo "No partitions captured; nothing to decode"; return; }

    # Resolve how to invoke pio_cov: prefer the pip-installed package
    # (pio-cov-side `pip install -e .` or PyPI install), fall back to
    # PYTHONPATH against a local checkout. The latter is for projects
    # iterating on pio-cov itself before it's pip-installed.
    local pio_cov_python=("python3")
    if ! python3 -c 'import pio_cov' 2>/dev/null; then
        local pio_cov_root="${PIO_COV_ROOT:-$HOME/e/pio-cov}"
        if [ ! -d "$pio_cov_root" ]; then
            echo "WARN: pio_cov not importable and PIO_COV_ROOT=$pio_cov_root not found; skipping decode"
            echo "       Install with: pip install pio-cov"
            return
        fi
        echo "pio_cov not pip-installed; using PYTHONPATH=$pio_cov_root"
        pio_cov_python=("env" "PYTHONPATH=$pio_cov_root" "python3")
    fi

    local out_dir="${COVERAGE_OUT_DIR:-$SCRIPT_DIR/coverage}"
    mkdir -p "$out_dir"

    local merged_args=()
    for spec in "${COV_RUNS[@]}"; do
        local backend="${spec%%|*}"
        local rest="${spec#*|}"
        local env_name="${rest%%|*}"
        local partition="${rest#*|}"
        local cov_path=".pio/build/$env_name/$partition.cov"
        local info_path="$out_dir/$partition.info"

        if [ ! -f "$cov_path" ]; then
            echo "WARN: $cov_path missing — skipping decode for $partition"
            continue
        fi

        echo "--- decode $partition ($backend) ---"
        if [ "$backend" = "gcov" ]; then
            "${pio_cov_python[@]}" -m pio_cov lcov \
                --build-dir ".pio/build/$env_name" \
                --log "$cov_path" \
                --output "$info_path"
        else
            local elf_path=".pio/build/$env_name/firmware-$partition.elf"
            if [ ! -f "$elf_path" ]; then
                echo "WARN: snapshot ELF $elf_path missing — skipping $partition"
                continue
            fi
            "${pio_cov_python[@]}" -m pio_cov trace-pc \
                --elf "$elf_path" \
                --log "$cov_path" \
                --output "$info_path"
        fi
        merged_args+=(-a "$info_path")
    done

    if [ "${#merged_args[@]}" -gt 0 ]; then
        local combined="$out_dir/device.info"
        lcov "${merged_args[@]}" -o "$combined" \
            --ignore-errors inconsistent,inconsistent,unsupported,format,format \
            >/dev/null
        echo
        echo "========================================="
        echo "Combined device coverage → $combined"
        echo "========================================="
        lcov --summary "$combined" \
            --ignore-errors inconsistent,inconsistent,unsupported 2>&1 | tail -8
    fi
}

if [ "$COVERAGE_MODE" = "1" ]; then
    COV_RUNS=()
    run_coverage_partition gcov    test_fixtures
    run_coverage_partition tracepc test_units_a
    run_coverage_partition tracepc test_units_b
    run_coverage_partition tracepc test_units_c

    decode_and_merge_coverage
else
    # Build each interface environment
    run_env serial
    run_env i2c
    run_env both

    # Verify no-interface guard
    run_env nointerface expect_fail
fi

echo "========================================="
echo "Results: $passed passed, $failed failed"
echo "========================================="

[ "$failed" -eq 0 ]
