"""Pre-build script: exclude avr-libstdcpp source files from compilation.

avr-libstdcpp's .cc source files use C++17 features (is_same_v) that
avr-g++ 7.3 doesn't support. We only need the headers for type_traits,
optional, string_view, etc. — no compiled library code.
"""
Import("env")

# PlatformIO compiles library source files found in src/.
# Override the library builder to skip avr-libstdcpp sources.
for lb in env.GetLibBuilders():
    if "avr-libstdcpp" in lb.name:
        # Remove all source files — treat as header-only
        lb.env.Replace(SRC_FILTER=["-<*>"])
        break
