#!/usr/bin/env bash
# Pin configuration for the firmware integration test.
#
# Source this before running pio:
#
#   source env.sh                                    # ESP32-S3 DevKitC defaults
#   source env.sh --rx=21 --tx=47 --sda=39 --scl=38 # custom pins
#   pio test -e esp32s3
#
# The pins are injected as PLATFORMIO_BUILD_FLAGS so platformio.ini
# stays board-agnostic.

set -euo pipefail

# Defaults — ESP32-S3 DevKitC-1 common header pins
NOTECARD_SERIAL_RX=18
NOTECARD_SERIAL_TX=17
NOTECARD_I2C_SDA=1
NOTECARD_I2C_SCL=2

for arg in "$@"; do
    case "$arg" in
        --rx=*)  NOTECARD_SERIAL_RX="${arg#*=}" ;;
        --tx=*)  NOTECARD_SERIAL_TX="${arg#*=}" ;;
        --sda=*) NOTECARD_I2C_SDA="${arg#*=}" ;;
        --scl=*) NOTECARD_I2C_SCL="${arg#*=}" ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Usage: source env.sh [--rx=N] [--tx=N] [--sda=N] [--scl=N]" >&2
            return 1 2>/dev/null || exit 1
            ;;
    esac
done

export PLATFORMIO_BUILD_FLAGS="\
 -DNOTECARD_SERIAL_RX=$NOTECARD_SERIAL_RX \
 -DNOTECARD_SERIAL_TX=$NOTECARD_SERIAL_TX \
 -DNOTECARD_I2C_SDA=$NOTECARD_I2C_SDA \
 -DNOTECARD_I2C_SCL=$NOTECARD_I2C_SCL"

echo "Pin configuration:"
echo "  Serial: RX=$NOTECARD_SERIAL_RX TX=$NOTECARD_SERIAL_TX"
echo "  I2C:    SDA=$NOTECARD_I2C_SDA SCL=$NOTECARD_I2C_SCL"
echo "  PLATFORMIO_BUILD_FLAGS=$PLATFORMIO_BUILD_FLAGS"
