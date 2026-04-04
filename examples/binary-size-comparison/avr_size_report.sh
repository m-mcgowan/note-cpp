#!/bin/bash
# AVR binary size comparison: note-c vs note-cpp
# Run from examples/binary-size-comparison/
set -e

NM=$(find ~/.platformio/packages/toolchain-atmelavr -name avr-nm 2>/dev/null | head -1)
if [ -z "$NM" ]; then
    echo "Error: avr-nm not found. Install PlatformIO atmelavr toolchain."
    exit 1
fi

echo "Building AVR targets..."
pio run -e avr-notec -e avr-notecpp 2>&1 | grep -E 'RAM|Flash|SUCCESS|FAILED|Environment'

echo ""
echo "═══════════════════════════════════════════════════════════"
echo " AVR Binary Size Report"
echo "═══════════════════════════════════════════════════════════"

for env in avr-notec avr-notecpp; do
    ELF=".pio/build/$env/firmware.elf"
    if [ ! -f "$ELF" ]; then
        echo "  $env: BUILD FAILED"
        continue
    fi

    echo ""
    echo "── $env ──"

    # Summary
    avr-size "$ELF" 2>/dev/null || $NM -S -t d "$ELF" | awk '
        / [Dd] / { data += $2 }
        / [Bb] / { bss += $2 }
        / [Tt] / { text += $2 }
        END { printf "  text=%d  data=%d  bss=%d  total_ram=%d\n", text, data, bss, data+bss }
    '

    echo "  .data (initialized, flash+RAM):"
    $NM -S -t d "$ELF" | grep ' [Dd] ' | awk '{printf "    %6d  %s\n", $2, $4}' | sort -rn | head -10

    echo "  .bss (uninitialized, RAM only):"
    $NM -S -t d "$ELF" | grep ' [Bb] ' | awk '{printf "    %6d  %s\n", $2, $4}' | sort -rn | head -10

    echo "  .text top 10 (flash):"
    $NM -S -t d "$ELF" | grep ' [Tt] ' | awk '{printf "    %6d  %s\n", $2, $4}' | sort -rn | head -10
done
