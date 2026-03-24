"""PlatformIO pre-build script: override pin definitions from environment variables.

Environment variables (set by env.sh or boards.sh) take precedence over the
default pin values in platformio.ini. Only pins that are already defined in the
current environment's build_flags are overridden — env vars do not add new pins.
This means `pio test -e i2c` stays I2C-only even if serial env vars are set.

Recognized variables:
    RX1, TX1                (Arduino serial pin defines)
    NOTECARD_I2C_SDA, NOTECARD_I2C_SCL
"""
Import("env")
import os

PIN_VARS = [
    "RX1",
    "TX1",
    "NOTECARD_I2C_SDA",
    "NOTECARD_I2C_SCL",
]

# Find which pin vars are already in this env's build_flags.
flags = [str(f) for f in env.get("BUILD_FLAGS", [])]
present = set()
for f in flags:
    for v in PIN_VARS:
        if f.startswith("-D" + v + "=") or f == "-D" + v:
            present.add(v)

# Override only pins that are already defined in this env.
overrides = {}
for var in PIN_VARS:
    if var not in present:
        continue
    val = os.environ.get(var)
    if val is not None:
        overrides[var] = val

if overrides:
    # Remove existing flags for overridden pins.
    filtered = [f for f in flags
                if not any(f.startswith("-D" + v + "=") or f == "-D" + v
                           for v in overrides)]
    env.Replace(BUILD_FLAGS=filtered)

    # Add the environment variable overrides.
    for var, val in overrides.items():
        env.Append(BUILD_FLAGS=["-D{}={}".format(var, val)])

    print("Pin overrides from environment: " +
          ", ".join("{}={}".format(k, v) for k, v in overrides.items()))
