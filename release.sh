#!/usr/bin/env bash
# release.sh — tag and push a validated release
#
# Usage:
#   ./release.sh <version>
#
# Prerequisites:
#   - validate-release.sh must have passed for this version and commit
#   - CHANGELOG.md must have a ## [<version>] section
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
RESULTS_FILE="$ROOT/.release-validation/results.json"

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    echo "Usage: ./release.sh <version>"
    exit 1
fi

TAG="v$VERSION"

# ── Check validation results ────────────────────────────────────────────────

if [ ! -f "$RESULTS_FILE" ]; then
    echo "ERROR: No validation results found."
    echo "  Run: ./validate-release.sh $VERSION"
    exit 1
fi

results_version=$(jq -r '.version' "$RESULTS_FILE")
if [ "$results_version" != "$VERSION" ]; then
    echo "ERROR: Validation was for v$results_version, not v$VERSION."
    echo "  Run: ./validate-release.sh $VERSION"
    exit 1
fi

results_commit=$(jq -r '.commit' "$RESULTS_FILE")
head_commit=$(git rev-parse HEAD)
if [ "$results_commit" != "$head_commit" ]; then
    echo "ERROR: Validation was for commit ${results_commit:0:12}, but HEAD is ${head_commit:0:12}."
    echo "  Re-run: ./validate-release.sh $VERSION"
    exit 1
fi

# Check all steps passed
failed_steps=$(jq -r '.steps[] | select(.status != "pass") | .name' "$RESULTS_FILE")
if [ -n "$failed_steps" ]; then
    echo "ERROR: The following steps did not pass:"
    echo "$failed_steps" | sed 's/^/  - /'
    echo "  Re-run: ./validate-release.sh $VERSION --from <step>"
    exit 1
fi

# Warn if host-only
host_only=$(jq -r '.host_only' "$RESULTS_FILE")
if [ "$host_only" = "true" ]; then
    echo "WARNING: Validation was run with --host-only (hardware not tested)."
    echo
fi

# ── Check changelog ─────────────────────────────────────────────────────────

if ! grep -q "^## \[$VERSION\]" "$ROOT/CHANGELOG.md"; then
    echo "ERROR: CHANGELOG.md has no '## [$VERSION]' section."
    echo "  Rename [Unreleased] to [$VERSION] and commit before releasing."
    exit 1
fi

# ── Extract changelog for tag message ───────────────────────────────────────

changelog_body=$(sed -n "/^## \[$VERSION\]/,/^## \[/{/^## \[$VERSION\]/d;/^## \[/d;p}" "$ROOT/CHANGELOG.md")

# ── Confirm ─────────────────────────────────────────────────────────────────

echo "Ready to release:"
echo "  Version:  $VERSION"
echo "  Tag:      $TAG"
echo "  Commit:   ${head_commit:0:12}"
echo "  Host-only: $host_only"
echo
echo "Changelog:"
echo "$changelog_body" | head -20
echo
printf "Proceed? [y/N] "
read -r confirm
if [[ "$confirm" != [yY] ]]; then
    echo "Aborted."
    exit 1
fi

# ── Tag and push ────────────────────────────────────────────────────────────

echo
echo "Creating tag $TAG..."
git tag -a "$TAG" -m "$(printf "Release %s\n\n%s" "$TAG" "$changelog_body")"

echo "Pushing tag to origin..."
git push origin "$TAG"

echo
echo "Done. GitHub Actions will create the release from this tag."
echo "  https://github.com/$(git remote get-url origin | sed 's|.*github.com[:/]||;s|\.git$||')/releases/tag/$TAG"
