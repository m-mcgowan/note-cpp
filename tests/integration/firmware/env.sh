#!/usr/bin/env bash
# Pin configuration for the firmware integration test.
#
# Source this before running pio to override pins defined in platformio.ini:
#
#   source env.sh                                    # Both interfaces, defaults
#   source env.sh --serial-only                      # Serial only, defaults
#   source env.sh --i2c-only                         # I2C only, defaults
#   source env.sh --rx=21 --tx=47 --sda=39 --scl=38 # Both, custom pins
#   source env.sh --i2c-only --sda=39 --scl=38      # I2C only, custom pins
#   pio test -e i2c
#
# Pin values are exported as individual environment variables which the
# set_pins.py pre-build script reads to override platformio.ini defaults.
# Unset variables leave the ini defaults in place.

set -euo pipefail

# Defaults — ESP32-S3 DevKitC-1 common header pins
_SERIAL_RX=18
_SERIAL_TX=17
_I2C_SDA=1
_I2C_SCL=2
_SERIAL=1
_I2C=1

for arg in "$@"; do
    case "$arg" in
        --rx=*)        _SERIAL_RX="${arg#*=}" ;;
        --tx=*)        _SERIAL_TX="${arg#*=}" ;;
        --sda=*)       _I2C_SDA="${arg#*=}" ;;
        --scl=*)       _I2C_SCL="${arg#*=}" ;;
        --serial-only) _I2C=0 ;;
        --i2c-only)    _SERIAL=0 ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Usage: source env.sh [--rx=N] [--tx=N] [--sda=N] [--scl=N] [--serial-only] [--i2c-only]" >&2
            return 1 2>/dev/null || exit 1
            ;;
    esac
done

# Export pin env vars for set_pins.py to pick up.
# Unset vars that aren't needed so the ini defaults aren't overridden.

if [ "$_SERIAL" = "1" ]; then
    export NOTECARD_SERIAL_RX="$_SERIAL_RX"
    export NOTECARD_SERIAL_TX="$_SERIAL_TX"
    echo "  Serial: RX=$_SERIAL_RX TX=$_SERIAL_TX"
else
    unset NOTECARD_SERIAL_RX 2>/dev/null || true
    unset NOTECARD_SERIAL_TX 2>/dev/null || true
    echo "  Serial: disabled"
fi

if [ "$_I2C" = "1" ]; then
    export NOTECARD_I2C_SDA="$_I2C_SDA"
    export NOTECARD_I2C_SCL="$_I2C_SCL"
    echo "  I2C:    SDA=$_I2C_SDA SCL=$_I2C_SCL"
else
    unset NOTECARD_I2C_SDA 2>/dev/null || true
    unset NOTECARD_I2C_SCL 2>/dev/null || true
    echo "  I2C:    disabled"
fi

# Clean up the old PLATFORMIO_BUILD_FLAGS if set from a previous env.sh version.
unset PLATFORMIO_BUILD_FLAGS 2>/dev/null || true
