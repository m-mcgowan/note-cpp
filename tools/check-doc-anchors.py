#!/usr/bin/env python3
"""Validate that #anchor fragments in markdown links resolve to real headings.

The existing markdown link check in verify-docs.sh verifies that the target
file exists but ignores the #fragment suffix, so renamed sections silently
break cross-references. This tool closes that gap.

For every markdown link of the form `[text](target.md#anchor)` (or the
same-file form `[text](#anchor)`), it parses the target file's headings,
slugifies them with GitHub's rules, and confirms the anchor matches one.

Slug rules (GitHub-flavored):
  - lowercase
  - drop everything that is not [a-z0-9 _-]
  - collapse spaces to single hyphens
  - duplicate slugs in the same file get -1, -2, ... suffixes

Explicit anchors (<a name="x"></a>, <a id="x"></a>) are also indexed.

Usage:
  python3 tools/check-doc-anchors.py             # check all user-facing docs
  python3 tools/check-doc-anchors.py --verbose   # list every checked link
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Same link regex as check-doc-reachability.py so both tools see the same set.
LINK_RE = re.compile(r"\[[A-Za-z0-9`][^\]]*\]\(([^)]+)\)")

# ATX heading lines: "## Title" — captures level and text.
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")

# Inline HTML anchors that authors sometimes add for stable cross-refs.
HTML_ANCHOR_RE = re.compile(r'<a\s+(?:name|id)="([^"]+)"', re.IGNORECASE)


def slugify(text: str) -> str:
    """GitHub-flavored heading slug."""
    # Strip inline markdown: backticks, emphasis markers, link wrappers.
    # Links: [text](url) → text
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    # Strip backticks, asterisks, underscores used as emphasis (keep _ inside words).
    text = text.replace("`", "")
    text = text.lower()
    # Keep only [a-z0-9], spaces, hyphens, underscores; drop the rest.
    text = re.sub(r"[^a-z0-9 _-]+", "", text)
    # Spaces (any run) → single hyphen.
    text = re.sub(r"\s+", "-", text).strip("-")
    return text


def heading_anchors(md_path: Path) -> set[str]:
    """Return all valid anchors for md_path: heading slugs + explicit anchors."""
    try:
        text = md_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return set()
    anchors: set[str] = set()
    counts: dict[str, int] = {}
    in_fence = False
    fence_marker = ""
    for line in text.splitlines():
        stripped = line.lstrip()
        # Track fenced code blocks so we don't pick up "## " inside them.
        if stripped.startswith("```") or stripped.startswith("~~~"):
            marker = stripped[:3]
            if not in_fence:
                in_fence = True
                fence_marker = marker
            elif stripped.startswith(fence_marker):
                in_fence = False
                fence_marker = ""
            continue
        if in_fence:
            continue
        m = HEADING_RE.match(line)
        if m:
            slug = slugify(m.group(2))
            if not slug:
                continue
            count = counts.get(slug, 0)
            anchors.add(slug if count == 0 else f"{slug}-{count}")
            counts[slug] = count + 1
        for am in HTML_ANCHOR_RE.finditer(line):
            anchors.add(am.group(1))
    return anchors


def iter_doc_links(md_path: Path):
    """Yield (target, fragment, line_number) for every link with a #fragment."""
    try:
        text = md_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return
    # Skip links that are inside fenced code blocks.
    in_fence = False
    fence_marker = ""
    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            marker = stripped[:3]
            if not in_fence:
                in_fence = True
                fence_marker = marker
            elif stripped.startswith(fence_marker):
                in_fence = False
                fence_marker = ""
            continue
        if in_fence:
            continue
        for match in LINK_RE.finditer(line):
            target = match.group(1).strip()
            if "#" not in target:
                continue
            # External / mailto / absolute API-reference URLs: skip.
            if target.startswith(("http://", "https://", "mailto:", "/api-reference/")):
                continue
            path, _, fragment = target.partition("#")
            if not fragment:
                continue
            yield path, fragment, lineno


def collect_md_files() -> list[Path]:
    """All user-facing .md files we should scan."""
    excludes_dirs = {"docs/superpowers", "docs/html", "docs/design-notes", "docs/doxygen"}
    docs = []
    for md in sorted((ROOT / "docs").rglob("*.md")):
        rel = md.relative_to(ROOT).as_posix()
        if any(rel.startswith(p + "/") for p in excludes_dirs):
            continue
        docs.append(md)
    for top in ("README.md", "CONTRIBUTING.md", "CHANGELOG.md"):
        p = ROOT / top
        if p.is_file():
            docs.append(p)
    return docs


_anchor_cache: dict[Path, set[str]] = {}


def anchors_for(md_path: Path) -> set[str]:
    resolved = md_path.resolve()
    if resolved not in _anchor_cache:
        _anchor_cache[resolved] = heading_anchors(resolved)
    return _anchor_cache[resolved]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print every checked link, not just broken ones",
    )
    args = parser.parse_args()

    md_files = collect_md_files()
    broken: list[tuple[Path, int, str, str, str]] = []  # src, lineno, path, fragment, hint
    checked = 0
    for md in md_files:
        for path, fragment, lineno in iter_doc_links(md):
            if path:
                target = (md.parent / path).resolve()
            else:
                target = md.resolve()
            if not target.is_file():
                # Existing link check covers missing files; don't double-report.
                continue
            if not str(target).endswith(".md"):
                continue
            checked += 1
            anchors = anchors_for(target)
            if fragment in anchors:
                if args.verbose:
                    rel = md.relative_to(ROOT).as_posix()
                    trel = target.relative_to(ROOT).as_posix()
                    print(f"  OK  {rel}:{lineno} -> {trel}#{fragment}")
                continue
            # Build a "did you mean" hint: closest by simple prefix/substring.
            hint = ""
            candidates = [a for a in sorted(anchors) if fragment[:6] and fragment[:6] in a]
            if candidates:
                hint = f"  closest: {', '.join(candidates[:3])}"
            broken.append((md, lineno, path, fragment, hint))

    print(f"Anchor links checked: {checked}")
    print(f"Broken: {len(broken)}")
    if broken:
        print()
        print("Broken anchor links:")
        for src, lineno, path, fragment, hint in broken:
            rel = src.relative_to(ROOT).as_posix()
            target_display = path if path else "(same file)"
            print(f"  {rel}:{lineno}")
            print(f"    -> {target_display}#{fragment}")
            if hint:
                print(f"   {hint}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
