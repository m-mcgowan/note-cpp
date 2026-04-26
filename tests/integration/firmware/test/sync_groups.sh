#!/usr/bin/env bash
# Re-create test_units_*/ symlinks from groups.tsv.
#
# Run after editing groups.tsv to rebalance which portable test files
# go in which PIO test group. Idempotent — drops stale symlinks under
# test_units_*/ that aren't in the manifest, then writes/replaces all
# entries from the manifest. Files at test/ top-level (main.cpp,
# test_custom_runner.py, this script) and test_fixtures/ are untouched.
#
# Usage:  ./sync_groups.sh
#
# Each symlink target path is `../../../../<file>`, resolving to
# tests/<file> from tests/integration/firmware/test/test_units_<x>/.

set -euo pipefail
cd "$(dirname "$0")"

MANIFEST=groups.tsv

# Clean stale symlinks under test_units_*/ — anything not in the manifest.
for dir in test_units_*/; do
    [ -d "$dir" ] || continue
    for f in "$dir"*.cpp; do
        [ -L "$f" ] || continue
        base=$(basename "$f")
        group=${dir%/}
        group=${group#test_}  # strip "test_" prefix; e.g. "test_units_a" -> "units_a"
        if ! awk -v g="$group" -v b="$base" '$1==g && $2==b {found=1} END {exit !found}' "$MANIFEST"; then
            rm -f "$f"
        fi
    done
done

# Create symlinks per manifest.
declare -A seen_groups=()
while IFS=$'\t' read -r group file; do
    case "$group" in ''|';'*) continue ;; esac  # skip blank/comment lines
    seen_groups[$group]=1
    mkdir -p "test_$group"
    ln -sf "../../../../$file" "test_$group/$file"
done < "$MANIFEST"

# Report.
echo "Synced symlinks for groups: ${!seen_groups[*]}"
for group in "${!seen_groups[@]}"; do
    n=$(ls -1 "test_$group/"*.cpp 2>/dev/null | wc -l | tr -d ' ')
    echo "  test_$group/: $n files"
done
