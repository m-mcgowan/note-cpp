#!/usr/bin/env bash
# CI script for the integration test firmware.
#
# Verifies that each PlatformIO environment builds correctly and that the
# no-interface guard fires a compilation error.
#
# Usage:
#   ./ci.sh              # build-only (CI, no hardware)
#   ./ci.sh --test       # build + upload + run on hardware
#
# Hardware test requires:
#   source boards.sh 1.9   # or your board revision
#   ./ci.sh --test --upload-port /dev/cu.usbmodem...
#
# Any extra arguments after --test are forwarded to `pio test`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RUN_TESTS=0
PIO_EXTRA_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --test) RUN_TESTS=1 ;;
        *)      PIO_EXTRA_ARGS+=("$arg") ;;
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

# Build each interface environment
run_env serial
run_env i2c
run_env both

# Verify no-interface guard
run_env nointerface expect_fail

echo "========================================="
echo "Results: $passed passed, $failed failed"
echo "========================================="

[ "$failed" -eq 0 ]
