#!/usr/bin/env bash
# Verify documentation: embedded code snippets, migration table alignment,
# and internal markdown links.
# Called by pre-push hook and ci.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Verify code snippets in all markdown files with <!-- snippet: --> markers
echo "=== Snippet verification ==="
SNIPPET_MDS=$(find "$ROOT" -name 'README.md' -not -path '*/.pio/*' -not -path '*/node_modules/*' 2>/dev/null || true)
python3 "$ROOT/tools/inject-snippets.py" --check $SNIPPET_MDS
echo "  OK"

# Verify migration guide table alignment
MIGRATION="$ROOT/docs/guides/migration-from-note-arduino.md"
if [ -f "$MIGRATION" ]; then
    echo "=== Migration table alignment ==="
    python3 "$ROOT/tools/pad_migration_tables.py" --check
fi

# Verify internal markdown links
echo "=== Markdown link check ==="
LINK_ERRORS=0
while IFS= read -r mdfile; do
    dir="$(dirname "$mdfile")"
    # Extract markdown links: [text](target) — skip http, mailto, anchors
    # Match [text](target) links — require non-empty text before ]( to avoid
    # matching code like fn(auto& b) or array](index).
    # Match [text](target) links. Require alphabetic char in text to skip
    # code patterns like [](auto& b) or [0].
    grep -oE '\[[A-Za-z][^]]*\]\([^)]+\)' "$mdfile" 2>/dev/null | \
        sed 's/^.*](//' | sed 's/)$//' | \
        grep -v '^http' | grep -v '^mailto' | grep -v '^#' | \
        grep -v '^/api-reference/' | \
    while IFS= read -r target; do
        # Strip anchor
        path="${target%%#*}"
        [ -z "$path" ] && continue
        # Resolve relative to the markdown file's directory
        resolved="$dir/$path"
        if [ ! -e "$resolved" ]; then
            echo "  BROKEN: $mdfile → $target"
            echo "    (resolved to $resolved)"
            # Write to a temp file since we're in a subshell
            echo 1 > /tmp/note-cpp-link-check-failed
        fi
    done || true
done < <(find "$ROOT/docs" -name '*.md' -not -path '*/.pio/*' \
    -not -path '*/node_modules/*' -not -path '*/superpowers/*' 2>/dev/null;
    echo "$ROOT/README.md"; echo "$ROOT/CHANGELOG.md")

if [ -f /tmp/note-cpp-link-check-failed ]; then
    rm -f /tmp/note-cpp-link-check-failed
    echo "  Broken links found!"
    exit 1
fi
rm -f /tmp/note-cpp-link-check-failed
echo "  OK"
