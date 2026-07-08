#!/usr/bin/env bash
# release-notes.sh — emit GitHub release notes for a version.
#
# Combines two sources:
#   1. The CHANGELOG.md section for the version (user-facing changes).
#   2. A validation summary from .release-validation/results.json — the exact
#      steps validate-release.sh ran (host suite + hardware-in-the-loop + AVR
#      runtime), so the release records what was actually tested and on what.
#
# results.json is developer-local (gitignored), so this must run where
# validation happened — i.e. from release.sh, not the GitHub workflow. When
# results.json is absent, only the changelog is emitted (the workflow fallback).
#
# Usage: tools/release-notes.sh <version>   # writes markdown to stdout
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:?usage: release-notes.sh <version>}"
RESULTS="$ROOT/.release-validation/results.json"

# ── Changelog section (between "## [VERSION]" and the next "## [") ────────────
awk -v hdr="## [$VERSION]" '
    index($0, hdr) == 1 { inside = 1; next }
    inside && /^## \[/ { exit }
    inside { print }
' "$ROOT/CHANGELOG.md"

# ── Validation summary from results.json ─────────────────────────────────────
[ -f "$RESULTS" ] || exit 0
command -v jq >/dev/null 2>&1 || exit 0

results_version=$(jq -r '.version' "$RESULTS")
[ "$results_version" = "$VERSION" ] || exit 0   # stale results for another version

commit=$(jq -r '.commit' "$RESULTS")
host_only=$(jq -r '.host_only' "$RESULTS")
completed=$(jq -r '.completed // ""' "$RESULTS")
total=$(jq '.steps | length' "$RESULTS")
passed=$(jq '[.steps[] | select(.status == "pass")] | length' "$RESULTS")
total_s=$(jq '[.steps[].duration_s] | add' "$RESULTS")
has_hw=$(jq '[.steps[] | select(.name | startswith("hw-"))] | length > 0' "$RESULTS")
has_avr=$(jq '[.steps[] | select(.name == "avr-runtime")] | length > 0' "$RESULTS")

if [ "$host_only" = "true" ]; then mode="host-only"; else mode="full (hardware-in-the-loop)"; fi

echo
echo "## Validation"
echo
printf '`%s` · %s · **%s/%s** steps passed · %dm%02ds' \
    "${commit:0:7}" "$mode" "$passed" "$total" $((total_s / 60)) $((total_s % 60))
[ -n "$completed" ] && printf ' · %s' "$completed"
echo
echo

echo "<details><summary>Validation steps</summary>"
echo
echo "| Step | Result | Duration |"
echo "|---|---|:--:|"
jq -r '.steps[] | "| \(.name) | \(if .status == "pass" then "✅" else "❌ " + .status end) | \(.duration_s)s |"' "$RESULTS"
echo
echo "</details>"

# Highlight what hardware/simulation actually ran.
if [ "$has_hw" = "true" ] || [ "$has_avr" = "true" ]; then
    echo
    joined=""
    [ "$has_hw" = "true" ]  && joined="ESP32-S3 serial + I²C (native + Arduino) against a real Notecard"
    [ "$has_avr" = "true" ] && joined="${joined:+$joined; }AVR ATmega328P runtime via Wokwi (full \`execute()\` cycle)"
    echo "**Hardware & simulation:** ${joined}."
fi
