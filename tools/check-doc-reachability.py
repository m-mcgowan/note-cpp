#!/usr/bin/env python3
"""Check that every user-facing .md doc is reachable from README.md.

Starts BFS from README.md, follows internal .md links (transitively), and
reports any docs/*.md (and subdirectories) that aren't in the reachable set.

Excludes by default:
  - docs/superpowers/, docs/html/, docs/design-notes/   (working artifacts)
  - docs/doxygen/                                       (separate site)
  - docs/internal/                                      (contributor docs;
    reachable via docs/README.md § Contributing only — included if --strict)

Usage:
  python3 tools/check-doc-reachability.py             # report orphans, exit 1 if any
  python3 tools/check-doc-reachability.py --strict    # also require internal/ docs reachable
  python3 tools/check-doc-reachability.py --verbose   # print the reachable set too
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Entrypoints: docs reachable from these are considered "reached".
ENTRYPOINTS = [
    ROOT / "README.md",
    ROOT / "docs" / "README.md",
]

# Default-excluded paths (working artifacts, generated sites, contributor docs).
DEFAULT_EXCLUDES = {
    "docs/superpowers",
    "docs/html",
    "docs/design-notes",
    "docs/doxygen",
    "docs/internal",
}

STRICT_EXCLUDES = {
    "docs/superpowers",
    "docs/html",
    "docs/design-notes",
    "docs/doxygen",
}

# Matches [text](target) — text must start with a letter/digit or backtick
# to skip code-fence patterns like [](auto& b) or array[0]. Backtick allows
# inline-code link text like [`avr-flash-strings.md`](...).
LINK_RE = re.compile(r"\[[A-Za-z0-9`][^\]]*\]\(([^)]+)\)")


def extract_md_links(md_path: Path) -> list[Path]:
    """Return absolute Paths to .md files this doc links to."""
    try:
        text = md_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    results = []
    for match in LINK_RE.finditer(text):
        target = match.group(1).strip()
        # Skip external, mailto, in-page anchors, and absolute API-reference URLs
        if target.startswith(("http://", "https://", "mailto:", "#", "/api-reference/")):
            continue
        # Strip in-page anchor
        target = target.split("#", 1)[0]
        if not target:
            continue
        # Only follow .md targets
        if not target.endswith(".md"):
            continue
        resolved = (md_path.parent / target).resolve()
        if resolved.is_file():
            results.append(resolved)
    return results


def reachable_from(entrypoints: list[Path]) -> set[Path]:
    """BFS from entrypoints; return absolute Paths of all reached .md files."""
    seen: set[Path] = set()
    frontier: list[Path] = []
    for ep in entrypoints:
        if ep.is_file():
            ep_abs = ep.resolve()
            seen.add(ep_abs)
            frontier.append(ep_abs)
    while frontier:
        current = frontier.pop()
        for link in extract_md_links(current):
            if link not in seen:
                seen.add(link)
                frontier.append(link)
    return seen


def all_user_docs(excludes: set[str]) -> set[Path]:
    """Find every .md under docs/ that isn't excluded."""
    docs = set()
    for md in (ROOT / "docs").rglob("*.md"):
        rel = md.relative_to(ROOT).as_posix()
        if any(rel.startswith(prefix + "/") or rel == prefix for prefix in excludes):
            continue
        docs.add(md.resolve())
    return docs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Require internal/ docs to be reachable too (default: skip internal/)",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print the reachable set in addition to orphans",
    )
    args = parser.parse_args()

    excludes = STRICT_EXCLUDES if args.strict else DEFAULT_EXCLUDES
    user_docs = all_user_docs(excludes)
    reached = reachable_from(ENTRYPOINTS)
    orphans = sorted(user_docs - reached)

    if args.verbose:
        print(f"Entrypoints ({len(ENTRYPOINTS)}):")
        for ep in ENTRYPOINTS:
            rel = ep.relative_to(ROOT).as_posix() if ep.exists() else f"{ep} (missing)"
            print(f"  {rel}")
        print(f"\nReachable .md files: {len(reached)}")
        for r in sorted(reached):
            try:
                print(f"  {r.relative_to(ROOT).as_posix()}")
            except ValueError:
                print(f"  {r}")
        print()

    print(f"User-facing docs scanned: {len(user_docs)}")
    print(f"Reachable from README.md: {len(user_docs & reached)}")
    print(f"Orphans: {len(orphans)}")

    if orphans:
        print()
        print("Docs not reachable from README.md or docs/README.md:")
        for o in orphans:
            print(f"  {o.relative_to(ROOT).as_posix()}")
        print()
        print("Fix by:")
        print("  - adding a link from a reachable doc, or")
        print("  - moving the file to docs/internal/ (or another excluded path), or")
        print("  - deleting if obsolete")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
