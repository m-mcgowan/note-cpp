#!/usr/bin/env bash
# validate-release.sh — run all release validation steps
#
# Usage:
#   ./validate-release.sh <version>                 # Run all steps
#   ./validate-release.sh <version> --from <step>   # Resume from a named step
#   ./validate-release.sh <version> --step <step>   # Run a single step
#   ./validate-release.sh <version> --list          # Show steps and their status
#   ./validate-release.sh <version> --host-only     # Skip hardware steps
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$ROOT/.release-validation"
RESULTS_FILE="$RESULTS_DIR/results.json"

# ── Step functions ──────────────────────────────────────────────────────────
# Each returns 0 on success, non-zero on failure.
# The VERSION variable is set before any step runs.

step_version_check() {
    local lib_props_ver lib_json_ver
    lib_props_ver=$(grep '^version=' "$ROOT/library.properties" | cut -d= -f2)
    lib_json_ver=$(jq -r '.version' "$ROOT/library.json")

    local ok=0
    if [ "$lib_props_ver" != "$VERSION" ]; then
        echo "FAIL: library.properties has version '$lib_props_ver', expected '$VERSION'"
        ok=1
    else
        echo "  library.properties: $lib_props_ver OK"
    fi
    if [ "$lib_json_ver" != "$VERSION" ]; then
        echo "FAIL: library.json has version '$lib_json_ver', expected '$VERSION'"
        ok=1
    else
        echo "  library.json: $lib_json_ver OK"
    fi
    return $ok
}

step_changelog_check() {
    if grep -q "^## \[$VERSION\]" "$ROOT/CHANGELOG.md"; then
        echo "  CHANGELOG.md has entry for [$VERSION]"
    else
        echo "FAIL: CHANGELOG.md has no '## [$VERSION]' section."
        echo "  Rename [Unreleased] to [$VERSION] and add a date before validating."
        return 1
    fi
}

step_codegen() {
    local PYTHON=python3
    if [ -f "$ROOT/.venv/bin/python3" ]; then
        PYTHON="$ROOT/.venv/bin/python3"
    fi

    "$PYTHON" "$ROOT/tools/codegen/generate.py" "$ROOT/notecard-api.openapi.json" \
        -o "$ROOT/include/note/api" \
        --api "$ROOT/include/note/api.hpp" \
        --test-dir "$ROOT/tests"

    if ! git diff --quiet -- \
        "$ROOT/include/note/api/" \
        "$ROOT/include/note/api.hpp" \
        "$ROOT/tests/test_samples.cpp" \
        "$ROOT/tests/test_api_context.cpp" \
        "$ROOT/tests/test_endpoint_coverage.cpp"; then
        echo "FAIL: Generated files are out of date after running codegen."
        git diff --stat -- \
            "$ROOT/include/note/api/" \
            "$ROOT/include/note/api.hpp" \
            "$ROOT/tests/test_samples.cpp" \
            "$ROOT/tests/test_api_context.cpp" \
            "$ROOT/tests/test_endpoint_coverage.cpp"
        return 1
    fi
    echo "  Generated files are up to date."
}

step_host_tests() {
    "$ROOT/ci.sh"
}

step_full_checks() {
    "$ROOT/ci.sh" --full
}

step_integrations() {
    "$ROOT/ci.sh" --integrations
}

step_fuzz() {
    "$ROOT/ci.sh" --fuzz
}

step_mutate() {
    # Test-strength audit (mutation testing) of the correctness-critical
    # headers. Runs the PoC target set; exits 0 even with surviving mutants
    # (they need human triage, not a hard fail), so this gates on the harness
    # running and every target's baseline tests being green. Self-skips when
    # clang.cindex / a compiler is unavailable.
    "$ROOT/ci.sh" --mutate
}

step_coverage() {
    "$ROOT/ci.sh" --coverage
}

step_docs() {
    "$ROOT/ci.sh" --docs
}

step_pio_build() {
    (cd "$ROOT/tests/integration/firmware" && ./ci.sh)
}

step_arduino_build() {
    local esp32_fqbn="esp32:esp32:esp32s3:CDCOnBoot=cdc"
    local swan_fqbn="STMicroelectronics:stm32:Blues:pnum=SWAN_R5"

    for sketch in quickstart readme_snippets serial_basic i2c_basic; do
        echo "--- $sketch (ESP32-S3) ---"
        arduino-cli compile --fqbn "$esp32_fqbn" \
            "$ROOT/examples/arduino/$sketch"

        echo "--- $sketch (Blues Swan) ---"
        arduino-cli compile --fqbn "$swan_fqbn" \
            "$ROOT/examples/arduino/$sketch"
    done
}

# ── Hardware step functions ─────────────────────────────────────────────────

step_hw_esp32_serial() {
    local port
    port=$(usb-device port "$ESP32_DEVICE")
    (cd "$ROOT/tests/integration/firmware" && \
        source boards.sh "$ESP32_BOARD" --serial-only && \
        pio test -e serial \
            --upload-port "$port" \
            --test-port "$port")
}

step_hw_esp32_i2c() {
    local port
    port=$(usb-device port "$ESP32_DEVICE")
    (cd "$ROOT/tests/integration/firmware" && \
        source boards.sh "$ESP32_BOARD" --i2c-only && \
        pio test -e i2c \
            --upload-port "$port" \
            --test-port "$port")
}

step_hw_esp32_arduino_serial() {
    local port
    port=$(usb-device port "$ESP32_DEVICE")
    arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
        --upload --port "$port" \
        "$ROOT/examples/arduino/serial_basic"
    echo "  Uploaded. Verifying serial output..."
    serial-monitor "$ESP32_DEVICE" --timeout 30 | grep -q "\[ok\] hub.set"
    echo "  serial_basic OK"
}

step_hw_esp32_arduino_i2c() {
    local port
    port=$(usb-device port "$ESP32_DEVICE")
    arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
        --upload --port "$port" \
        "$ROOT/examples/arduino/i2c_basic"
    echo "  Uploaded. Verifying serial output..."
    serial-monitor "$ESP32_DEVICE" --timeout 30 | grep -q "\[ok\] hub.set"
    echo "  i2c_basic OK"
}

step_hw_swan_arduino_serial() {
    local port
    port=$(usb-device port "$SWAN_DEVICE")
    arduino-cli compile --fqbn STMicroelectronics:stm32:Blues:pnum=SWAN_R5 \
        --upload --port "$port" \
        "$ROOT/examples/arduino/serial_basic"
    echo "  Uploaded. Verifying serial output..."
    serial-monitor "$SWAN_DEVICE" --timeout 30 | grep -q "\[ok\] hub.set"
    echo "  serial_basic OK"
}

step_avr_runtime() {
    # AVR runtime attestation. The AVR size check builds the Uno firmware but
    # never runs it; this step executes the smallest Uno build through the full
    # execute() request/response cycle on a Wokwi-simulated ATmega328P against
    # the mock Notecard chip, catching Uno-specific init/stack crashes that a
    # compile-only check can't. Needs no physical device (category: host).
    # Self-skips when the AVR toolchain, wokwi-cli, or the Wokwi token (.wokwi,
    # developer-local / gitignored) isn't available — e.g. in CI.
    if ! ls ~/.platformio/packages/toolchain-atmelavr* >/dev/null 2>&1; then
        echo "  SKIP: AVR toolchain not installed."; return 0
    fi
    if ! command -v wokwi-cli >/dev/null 2>&1; then
        echo "  SKIP: wokwi-cli not installed."; return 0
    fi
    if [ ! -f "$ROOT/.wokwi" ]; then
        echo "  SKIP: no Wokwi token (.wokwi) present."; return 0
    fi

    local bsc="$ROOT/tools/binary-size-comparison"
    echo "  Building avr-notecpp-scan-flash (smallest Uno build)..."
    pio run -d "$bsc" -e avr-notecpp-scan-flash >/dev/null

    echo "  Running Wokwi ATmega328P simulation..."
    local out
    # shellcheck disable=SC1091
    out=$( . "$ROOT/.wokwi"; cd "$bsc" && wokwi-cli --timeout 30000 . 2>&1 || true )

    # The sketch idles after its request sequence, so wokwi-cli ends on the
    # timeout (expected — no auto-stop scenario). Pass = the full cycle ran with
    # no crash/reboot: the final request (env.get) is reached and the reset
    # handshake occurs exactly once (a stack-overflow crash reboots the sketch,
    # repeating the handshake).
    local resets
    resets=$(echo "$out" | grep -c 'reset handshake')
    if ! echo "$out" | grep -q 'env.get'; then
        echo "$out" | tail -20
        echo "  FAIL: AVR sim did not complete the request sequence (env.get not reached)."
        return 1
    fi
    if [ "$resets" -ne 1 ]; then
        echo "$out" | tail -20
        echo "  FAIL: AVR sim rebooted ${resets}x — likely a stack overflow / crash."
        return 1
    fi
    echo "  AVR Uno runtime: full execute() cycle OK (no crash, single boot)."
}

# ── Step registry ───────────────────────────────────────────────────────────
# Format: "name|category|description|function"

STEPS=(
    "version-check|host|Version files match release version|step_version_check"
    "changelog-check|host|Changelog has entry for this version|step_changelog_check"
    "codegen|host|Generated code is up to date|step_codegen"
    "host-tests|host|CMake build + both test targets|step_host_tests"
    "full-checks|host|Headers, examples, version gating, target filtering|step_full_checks"
    "integrations|host|cjson / nlohmann / buffer backend tests|step_integrations"
    "coverage|host|Coverage thresholds enforced|step_coverage"
    "fuzz|host|JSON/JSONB parser fuzzing under ASan/UBSan|step_fuzz"
    "mutate|host|Mutation testing — test-strength audit|step_mutate"
    "docs|host|Doxygen build + link validation|step_docs"
    "pio-build|host|PlatformIO firmware builds (no hardware)|step_pio_build"
    "arduino-build|host|Arduino sketch compilation (no upload)|step_arduino_build"
    "hw-esp32-serial|hardware|Integration tests via serial on ESP32-S3|step_hw_esp32_serial"
    "hw-esp32-i2c|hardware|Integration tests via I2C on ESP32-S3|step_hw_esp32_i2c"
    "hw-esp32-arduino-serial|hardware|Arduino serial sketch on ESP32-S3|step_hw_esp32_arduino_serial"
    "hw-esp32-arduino-i2c|hardware|Arduino I2C sketch on ESP32-S3|step_hw_esp32_arduino_i2c"
    "hw-swan-arduino-serial|hardware|Arduino serial sketch on Swan|step_hw_swan_arduino_serial"
    "avr-runtime|host|AVR Uno runtime via Wokwi (full execute cycle, no crash)|step_avr_runtime"
)

# ── Hardware device configuration ───────────────────────────────────────────
# Override these with environment variables if your device names differ.
ESP32_DEVICE="${ESP32_DEVICE:-MPCB 1.9 Development}"
ESP32_BOARD="${ESP32_BOARD:-1.9}"
SWAN_DEVICE="${SWAN_DEVICE:-Blues Swan}"

# ── Helpers ─────────────────────────────────────────────────────────────────

step_name()  { echo "$1" | cut -d'|' -f1; }
step_cat()   { echo "$1" | cut -d'|' -f2; }
step_desc()  { echo "$1" | cut -d'|' -f3; }
step_func()  { echo "$1" | cut -d'|' -f4; }

find_step_index() {
    local target="$1"
    for i in "${!STEPS[@]}"; do
        if [ "$(step_name "${STEPS[$i]}")" = "$target" ]; then
            echo "$i"
            return 0
        fi
    done
    echo "ERROR: Unknown step '$target'" >&2
    echo "  Available steps:" >&2
    for s in "${STEPS[@]}"; do
        echo "    $(step_name "$s")" >&2
    done
    return 1
}

init_results() {
    mkdir -p "$RESULTS_DIR"
    local commit started
    commit=$(git -C "$ROOT" rev-parse HEAD)
    started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    cat > "$RESULTS_FILE" <<JSONEOF
{
  "version": "$VERSION",
  "commit": "$commit",
  "host_only": $HOST_ONLY_JSON,
  "started": "$started",
  "completed": null,
  "steps": []
}
JSONEOF
}

record_step() {
    local name="$1" status="$2" duration="$3"
    local tmp
    tmp=$(mktemp)
    jq --arg name "$name" --arg status "$status" --argjson dur "$duration" \
        '.steps += [{"name": $name, "status": $status, "duration_s": $dur}]' \
        "$RESULTS_FILE" > "$tmp" && mv "$tmp" "$RESULTS_FILE"
}

finalize_results() {
    local completed tmp
    completed=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    tmp=$(mktemp)
    jq --arg completed "$completed" '.completed = $completed' \
        "$RESULTS_FILE" > "$tmp" && mv "$tmp" "$RESULTS_FILE"
}

show_list() {
    # Load existing results if available
    local -A statuses=()
    if [ -f "$RESULTS_FILE" ]; then
        local results_version
        results_version=$(jq -r '.version' "$RESULTS_FILE")
        if [ "$results_version" = "$VERSION" ]; then
            while IFS= read -r line; do
                local n s
                n=$(echo "$line" | jq -r '.name')
                s=$(echo "$line" | jq -r '.status')
                statuses["$n"]="$s"
            done < <(jq -c '.steps[]' "$RESULTS_FILE")
        fi
    fi

    printf "\nRelease validation steps for v%s:\n\n" "$VERSION"
    printf "  %-30s %-10s %-10s %s\n" "STEP" "CATEGORY" "STATUS" "DESCRIPTION"
    printf "  %-30s %-10s %-10s %s\n" "----" "--------" "------" "-----------"
    for entry in "${STEPS[@]}"; do
        local n c d
        n=$(step_name "$entry")
        c=$(step_cat "$entry")
        d=$(step_desc "$entry")
        local st="${statuses[$n]:-pending}"
        printf "  %-30s %-10s %-10s %s\n" "$n" "$c" "$st" "$d"
    done
    echo
}

# ── Device locking ──────────────────────────────────────────────────────────

LOCKED_DEVICES=()

lock_devices() {
    echo "=== Acquiring device locks ==="
    local devices=("$ESP32_DEVICE")
    # Only lock Swan if the step is in the active set
    for entry in "${STEPS[@]}"; do
        if [ "$(step_name "$entry")" = "hw-swan-arduino-serial" ]; then
            devices+=("$SWAN_DEVICE")
            break
        fi
    done

    for dev in "${devices[@]}"; do
        echo "  Locking: $dev"
        usb-device checkout --pid $$ "$dev"
        LOCKED_DEVICES+=("$dev")
    done
    echo "  All devices locked."
    echo
}

unlock_devices() {
    if [ ${#LOCKED_DEVICES[@]} -eq 0 ]; then
        return
    fi
    echo
    echo "=== Releasing device locks ==="
    for dev in "${LOCKED_DEVICES[@]}"; do
        echo "  Unlocking: $dev"
        usb-device checkin --pid $$ "$dev" 2>/dev/null || true
    done
    LOCKED_DEVICES=()
}

# ── Trim results for --from mode ────────────────────────────────────────────

trim_results_to() {
    local start_idx="$1"
    # Build a JSON array of step names to keep (those before start_idx)
    local keep_names="["
    local first=true
    for ((j=0; j<start_idx; j++)); do
        if [ "$first" = true ]; then
            first=false
        else
            keep_names+=","
        fi
        keep_names+="\"$(step_name "${STEPS[$j]}")\""
    done
    keep_names+="]"
    local tmp
    tmp=$(mktemp)
    jq --argjson keep "$keep_names" \
        '.steps = [.steps[] | select(.name as $n | $keep | index($n) != null)]' \
        "$RESULTS_FILE" > "$tmp" && mv "$tmp" "$RESULTS_FILE"
}

# ── Main ────────────────────────────────────────────────────────────────────

usage() {
    cat <<EOF
Usage: ./validate-release.sh <version> [options]

Options:
  --from <step>    Resume from a named step
  --step <step>    Run a single step (no results tracking)
  --list           Show steps and their last status
  --host-only      Skip hardware steps

Steps are run in order. On failure, fix the issue and resume with --from.
EOF
}

# Parse args
VERSION=""
MODE="all"       # all | from | step | list
FROM_STEP=""
SINGLE_STEP=""
HOST_ONLY=false
HOST_ONLY_JSON="false"

while [ $# -gt 0 ]; do
    case "$1" in
        --from)
            MODE="from"
            FROM_STEP="${2:-}"
            [ -z "$FROM_STEP" ] && { usage; exit 1; }
            shift 2
            ;;
        --step)
            MODE="step"
            SINGLE_STEP="${2:-}"
            [ -z "$SINGLE_STEP" ] && { usage; exit 1; }
            shift 2
            ;;
        --list)
            MODE="list"
            shift
            ;;
        --host-only)
            HOST_ONLY=true
            HOST_ONLY_JSON="true"
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            if [ -z "$VERSION" ]; then
                VERSION="$1"
                shift
            else
                echo "ERROR: Unexpected argument '$1'" >&2
                usage
                exit 1
            fi
            ;;
    esac
done

if [ -z "$VERSION" ]; then
    usage
    exit 1
fi

# ── List mode ───────────────────────────────────────────────────────────────

if [ "$MODE" = "list" ]; then
    show_list
    exit 0
fi

# ── Single step mode (no results tracking — used by CI) ─────────────────────

if [ "$MODE" = "step" ]; then
    idx=$(find_step_index "$SINGLE_STEP")
    entry="${STEPS[$idx]}"
    echo "=== $(step_name "$entry"): $(step_desc "$entry") ==="
    "$(step_func "$entry")"
    exit $?
fi

# ── Full / resume mode ─────────────────────────────────────────────────────

echo "════════════════════════════════════════════════════════════════"
echo "  Release validation: v$VERSION"
echo "  Commit: $(git -C "$ROOT" rev-parse --short HEAD)"
echo "  Mode: $MODE  Host-only: $HOST_ONLY"
echo "════════════════════════════════════════════════════════════════"
echo

start_idx=0
if [ "$MODE" = "from" ]; then
    start_idx=$(find_step_index "$FROM_STEP")
fi

# Initialize results (or preserve prior results for --from)
if [ "$MODE" = "all" ]; then
    init_results
elif [ "$MODE" = "from" ]; then
    if [ ! -f "$RESULTS_FILE" ]; then
        init_results
    else
        trim_results_to "$start_idx"
    fi
fi

# Check if we need hardware steps
needs_hardware=false
for ((i=start_idx; i<${#STEPS[@]}; i++)); do
    if [ "$(step_cat "${STEPS[$i]}")" = "hardware" ]; then
        needs_hardware=true
        break
    fi
done

# Lock devices if we have hardware steps and aren't host-only
if [ "$needs_hardware" = true ] && [ "$HOST_ONLY" = false ]; then
    trap unlock_devices EXIT
    lock_devices
fi

# Run steps
total_start=$(date +%s)

for ((i=start_idx; i<${#STEPS[@]}; i++)); do
    entry="${STEPS[$i]}"
    name=$(step_name "$entry")
    cat=$(step_cat "$entry")
    desc=$(step_desc "$entry")
    func=$(step_func "$entry")

    # Skip hardware steps in host-only mode
    if [ "$HOST_ONLY" = true ] && [ "$cat" = "hardware" ]; then
        echo "--- SKIP: $name ($desc) [hardware, --host-only] ---"
        echo
        continue
    fi

    echo "=== [$((i+1))/${#STEPS[@]}] $name: $desc ==="
    step_start=$(date +%s)

    if "$func"; then
        duration=$(( $(date +%s) - step_start ))
        echo "--- PASS: $name (${duration}s) ---"
        record_step "$name" "pass" "$duration"
    else
        duration=$(( $(date +%s) - step_start ))
        echo "--- FAIL: $name (${duration}s) ---"
        record_step "$name" "fail" "$duration"
        finalize_results
        echo
        echo "Step '$name' failed. Fix the issue and resume with:"
        echo "  ./validate-release.sh $VERSION --from $name"
        exit 1
    fi
    echo
done

finalize_results
total_duration=$(( $(date +%s) - total_start ))

echo "════════════════════════════════════════════════════════════════"
echo "  All steps passed for v$VERSION (${total_duration}s)"
echo "════════════════════════════════════════════════════════════════"
