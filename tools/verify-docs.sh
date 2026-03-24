#!/usr/bin/env bash
# Verify documentation: embedded code snippets and migration table alignment.
# Called by pre-push hook and ci.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Verify embedded code snippets (embedme)
READMES=$(find "$ROOT/examples" -name 'README.md' -not -path '*/.pio/*' 2>/dev/null || true)
MIGRATION="$ROOT/docs/migration-from-note-arduino.md"
if [ -f "$MIGRATION" ]; then
    READMES="$READMES $MIGRATION"
fi

if [ -n "$READMES" ] && command -v npx >/dev/null 2>&1; then
    echo "=== Embedded doc verification ==="
    # shellcheck disable=SC2086
    npx -y embedme --verify $READMES
    echo "  OK"
fi

# Verify migration guide table alignment
if [ -f "$MIGRATION" ]; then
    echo "=== Migration table alignment ==="
    python3 "$ROOT/tools/pad_migration_tables.py" --check
fi
