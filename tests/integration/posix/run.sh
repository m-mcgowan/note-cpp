#!/usr/bin/env bash
# Smoke test for note::posix::PosixSerialHal against a real Notecard.
#
# By default this script SKIPS unless NOTE_CPP_POSIX_HW_TEST=1 is set, so
# it's safe to invoke from CI environments that don't have hardware.
#
# Usage:
#   NOTE_CPP_POSIX_HW_TEST=1 ./run.sh <usb-device-name>
#   NOTE_CPP_POSIX_HW_TEST=1 ./run.sh /dev/cu.usbmodem...
#
# Examples:
#   NOTE_CPP_POSIX_HW_TEST=1 ./run.sh "Notecard Alpha"
#   NOTE_CPP_POSIX_HW_TEST=1 ./run.sh /dev/ttyUSB0
#
# The target must be a Notecard exposed as a USB-serial device. On macOS
# that's /dev/cu.usbmodem*; on Linux typically /dev/ttyUSB* or /dev/ttyACM*.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ "${NOTE_CPP_POSIX_HW_TEST:-}" != "1" ]; then
    echo "posix integration: SKIPPED (set NOTE_CPP_POSIX_HW_TEST=1 to run)"
    exit 0
fi

ARG="${1:-}"
if [ -z "$ARG" ]; then
    echo "ERROR: pass a usb-device name or /dev/ path" >&2
    echo "  e.g. $0 'Notecard Alpha' or $0 /dev/cu.usbmodem14301" >&2
    exit 2
fi

# Resolve device path: explicit /dev/ path or usb-device name.
if [[ "$ARG" == /dev/* ]]; then
    DEVICE_PATH="$ARG"
else
    if ! command -v usb-device >/dev/null 2>&1; then
        echo "ERROR: '$ARG' is not a /dev/ path and usb-device is not installed" >&2
        exit 1
    fi
    DEVICE_PATH=$(usb-device port "$ARG") || {
        echo "ERROR: usb-device could not resolve '$ARG'" >&2
        exit 1
    }
fi

if [ ! -e "$DEVICE_PATH" ]; then
    echo "ERROR: device not found: $DEVICE_PATH" >&2
    exit 1
fi

echo "posix integration: using $DEVICE_PATH"

# Build the posix-hardware example fresh so we're testing the current tree.
BIN=/tmp/note-cpp-posix-hw
"${CXX:-c++}" -std=c++20 -I "$ROOT/include" \
    "$ROOT/examples/stdcpp/posix-hardware.cpp" -o "$BIN"

# 4 KB binary round-trip by default — exercises multi-segment transmit,
# COBS encode/decode, MD5 compute+verify: the real stress for the HAL.
# Override with BINARY_SIZE=0 to run only card.version, or any other size.
BINARY_SIZE="${BINARY_SIZE:-4096}"
BINARY_ARGS=()
if [ "$BINARY_SIZE" -gt 0 ]; then
    BINARY_ARGS=(--binary "$BINARY_SIZE")
fi

OUTPUT=$("$BIN" "${BINARY_ARGS[@]}" "$DEVICE_PATH" 2>&1)
echo "$OUTPUT"

# Assert card.version produced plausible output.
if ! echo "$OUTPUT" | grep -qE "^version: .+"; then
    echo "FAIL: 'version: <value>' not found in output" >&2
    exit 1
fi
if ! echo "$OUTPUT" | grep -qE "^device: .+"; then
    echo "FAIL: 'device: <value>' not found in output" >&2
    exit 1
fi

# Assert binary round-trip succeeded (unless explicitly skipped).
if [ "$BINARY_SIZE" -gt 0 ]; then
    if ! echo "$OUTPUT" | grep -qE "^binary: round-trip .* OK$"; then
        echo "FAIL: binary round-trip did not complete" >&2
        exit 1
    fi
fi

echo "posix integration: PASS"
