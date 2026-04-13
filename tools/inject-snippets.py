#!/usr/bin/env python3
"""Inject and verify markdown code snippets from source files.

Markdown markers:
  <!-- snippet:<name> <source-file> -->              Named marker
  <!-- snippet:<source-file>:<start>-<end> -->        Line range
  <!-- snippet:<name> <source-file> lines-after=3 --> With padding
  <!-- snippet:<name> <source-file> srclink -->        Add [source] link

Source markers:
  // readme:<name>  ...  // readme:end

Multiple snippet markers can precede a single code fence — their source
blocks are concatenated (separated by blank lines) into one code block.

Modes:
  --check   Verify snippets match source, exit 1 on mismatch (default)
  --inject  Rewrite markdown files in-place with injected code

After injection, named markers are annotated with the resolved line range
for GitHub linking:
  <!-- snippet:name file.cpp:17-25 -->

Usage:
    python3 tools/inject-snippets.py --check README.md
    python3 tools/inject-snippets.py --inject README.md
"""

import re
import sys
import textwrap
from pathlib import Path


def parse_snippet_comment(line: str) -> dict | None:
    """Parse a snippet comment into its components."""
    m = re.match(r'(?P<prefix>\s*)<!--\s*snippet:(?P<spec>[^>]+?)\s*-->', line)
    if not m:
        return None

    spec = m.group('spec').strip()
    prefix = m.group('prefix')

    # Extract boolean attributes
    srclink = 'srclink' in spec
    if srclink:
        spec = re.sub(r'\bsrclink\b', '', spec).strip()

    # Extract key=value attributes
    lines_after = 0
    la_match = re.search(r'\blines-after=(\d+)', spec)
    if la_match:
        lines_after = int(la_match.group(1))
        spec = spec[:la_match.start()].strip() + spec[la_match.end():].strip()
        spec = spec.strip()

    # Try: name file:start-end
    m2 = re.match(r'(\S+)\s+(\S+):(\d+)-(\d+)$', spec)
    if m2:
        return {
            'name': m2.group(1), 'file': m2.group(2),
            'start': int(m2.group(3)), 'end': int(m2.group(4)),
            'lines_after': lines_after, 'srclink': srclink, 'prefix': prefix,
        }

    # Try: name file
    m2 = re.match(r'(\S+)\s+(\S+)$', spec)
    if m2:
        name, filepath = m2.group(1), m2.group(2)
        if ':' not in name:
            return {
                'name': name, 'file': filepath,
                'start': None, 'end': None,
                'lines_after': lines_after, 'srclink': srclink, 'prefix': prefix,
            }

    # Try: file:start-end (no name)
    m2 = re.match(r'(\S+):(\d+)-(\d+)$', spec)
    if m2:
        return {
            'name': None, 'file': m2.group(1),
            'start': int(m2.group(2)), 'end': int(m2.group(3)),
            'lines_after': lines_after, 'srclink': srclink, 'prefix': prefix,
        }

    return None


def extract_source_blocks(source_path: Path) -> dict[str, list[tuple[list[str], int, int]]]:
    """Extract // readme:<name> ... // readme:end blocks from a source file.

    A name can appear multiple times — each occurrence is a separate range.
    Returns dict of name -> [(lines, start_line, end_line), ...].
    """
    blocks: dict[str, list[tuple[list[str], int, int]]] = {}
    current_name = None
    current_lines: list[str] = []
    start_line = 0

    all_lines = source_path.read_text().splitlines()
    for i, line in enumerate(all_lines, 1):
        stripped = line.strip()
        if m := re.match(r'//\s*readme:(\S+)', stripped):
            name = m.group(1)
            if name == 'end':
                if current_name:
                    blocks.setdefault(current_name, []).append(
                        (current_lines, start_line, i - 1))
                    current_name = None
                    current_lines = []
            else:
                current_name = name
                current_lines = []
                start_line = i + 1
        elif current_name is not None:
            current_lines.append(line)

    return blocks


def extract_lines_by_range(source_path: Path, start: int, end: int) -> list[str]:
    """Extract lines start..end (1-indexed, inclusive) from a file."""
    all_lines = source_path.read_text().splitlines()
    return all_lines[start - 1:end]


def normalize(lines: list[str]) -> list[str]:
    """Strip common leading whitespace and trailing blank lines."""
    text = textwrap.dedent('\n'.join(lines))
    result = text.splitlines()
    while result and result[-1].strip() == '':
        result.pop()
    while result and result[0].strip() == '':
        result.pop(0)
    return result


def format_snippet_comment(snippet: dict, start: int | None = None, end: int | None = None) -> str:
    """Rebuild the <!-- snippet:... --> comment with resolved line range."""
    parts = []
    if snippet['name']:
        parts.append(snippet['name'])

    filepath = snippet['file']
    s = start or snippet.get('start')
    e = end or snippet.get('end')
    if s and e:
        parts.append(f"{filepath}:{s}-{e}")
    else:
        parts.append(filepath)

    if snippet['lines_after'] > 0:
        parts.append(f"lines-after={snippet['lines_after']}")
    if snippet['srclink']:
        parts.append('srclink')

    return f"{snippet['prefix']}<!-- snippet:{' '.join(parts)} -->"


def make_source_link(filepath: str, start: int, end: int) -> str:
    """Build a markdown [source](file#L...) link."""
    return f"[source]({filepath}#L{start}-L{end})"


# Matches a previously-injected source link line.
SOURCE_LINK_RE = re.compile(r'^\s*\[source\]\(.*#L\d+-L\d+\)\s*$')


def process_markdown(md_path: Path, root: Path, inject: bool) -> int:
    """Process a markdown file. Returns number of errors."""
    lines = md_path.read_text().splitlines()
    errors = 0
    output_lines = []
    source_cache: dict[str, dict[str, tuple[list[str], int, int]]] = {}
    changed = False

    i = 0
    while i < len(lines):
        # Collect consecutive snippet markers
        snippets = []
        while i < len(lines):
            parsed = parse_snippet_comment(lines[i])
            if parsed:
                snippets.append((parsed, i))
                i += 1
            else:
                break

        if not snippets:
            output_lines.append(lines[i])
            i += 1
            continue

        # Skip blank lines to find code fence
        blank_lines = []
        while i < len(lines) and lines[i].strip() == '':
            blank_lines.append(lines[i])
            i += 1

        # Expect a code fence
        if i >= len(lines) or not lines[i].strip().startswith('```'):
            for _, orig_line_no in snippets:
                output_lines.append(lines[orig_line_no])
            output_lines.extend(blank_lines)
            print(f"  WARNING: snippet markers not followed by code fence near line {i + 1}")
            continue

        fence_open = lines[i]
        fence_lang = fence_open.strip().removeprefix('```').strip() or 'cpp'
        i += 1

        # Collect existing code block
        existing_code = []
        while i < len(lines) and not lines[i].strip().startswith('```'):
            existing_code.append(lines[i])
            i += 1
        fence_close = lines[i] if i < len(lines) else '```'
        i += 1  # skip closing fence

        # Consume any existing source link line after the fence
        if i < len(lines) and SOURCE_LINK_RE.match(lines[i]):
            i += 1

        # Resolve all snippet sources
        all_source_lines = []
        updated_comments = []
        has_error = False
        last_file = None
        last_start = None
        last_end = None
        want_srclink = False

        for parsed, orig_line_no in snippets:
            src_path = root / parsed['file']
            if not src_path.exists():
                print(f"  ERROR: {md_path.name}:{orig_line_no + 1} "
                      f"source file not found: {parsed['file']}")
                errors += 1
                has_error = True
                updated_comments.append(lines[orig_line_no])
                continue

            if parsed['srclink']:
                want_srclink = True

            if parsed['name']:
                # Named marker — resolve from source
                if parsed['file'] not in source_cache:
                    source_cache[parsed['file']] = extract_source_blocks(src_path)
                blocks = source_cache[parsed['file']]

                if parsed['name'] not in blocks:
                    print(f"  ERROR: {md_path.name}:{orig_line_no + 1} "
                          f"snippet '{parsed['name']}' not found in {parsed['file']}")
                    errors += 1
                    has_error = True
                    updated_comments.append(lines[orig_line_no])
                    continue

                ranges = blocks[parsed['name']]
                # Concatenate all ranges for this name (skip comment blocks etc.)
                combined_lines = []
                first_start = ranges[0][1]
                last_range_end = ranges[-1][2]
                for ri, (src_lines, start, end) in enumerate(ranges):
                    if ri > 0:
                        combined_lines.append('')  # blank line between ranges
                    combined_lines.extend(src_lines)
                resolved = normalize(combined_lines)
                updated_comments.append(
                    format_snippet_comment(parsed, start=first_start, end=last_range_end))
                last_file = parsed['file']
                last_start, last_end = first_start, last_range_end

            elif parsed['start'] is not None and parsed['end'] is not None:
                # Line range
                src_lines = extract_lines_by_range(
                    src_path, parsed['start'], parsed['end'])
                resolved = normalize(src_lines)
                updated_comments.append(
                    format_snippet_comment(parsed))
                last_file = parsed['file']
                last_start, last_end = parsed['start'], parsed['end']

            else:
                print(f"  ERROR: {md_path.name}:{orig_line_no + 1} "
                      f"could not parse snippet spec")
                errors += 1
                has_error = True
                updated_comments.append(lines[orig_line_no])
                continue

            if all_source_lines:
                all_source_lines.append('')  # blank line between combined blocks
            all_source_lines.extend(resolved)

        if has_error:
            for _, orig_line_no in snippets:
                output_lines.append(lines[orig_line_no])
            output_lines.extend(blank_lines)
            output_lines.append(fence_open)
            output_lines.extend(existing_code)
            output_lines.append(fence_close)
            continue

        # Padding
        padding = snippets[-1][0]['lines_after']

        # Compare
        expected = all_source_lines
        actual = normalize(existing_code)

        if expected != actual:
            if not inject:
                print(f"  MISMATCH: {md_path.name}:{snippets[0][1] + 1}")
                for parsed, _ in snippets:
                    name = parsed['name'] or f"{parsed['file']}:{parsed['start']}-{parsed['end']}"
                    print(f"    snippet: {name}")
                errors += 1
            else:
                changed = True

        # Emit updated content
        for comment in updated_comments:
            output_lines.append(comment)
        output_lines.extend(blank_lines)
        output_lines.append(f"```{fence_lang}")
        output_lines.extend(expected)
        for _ in range(padding):
            output_lines.append('')
        output_lines.append(fence_close)

        # Emit source link if requested
        if want_srclink and last_file and last_start and last_end:
            output_lines.append(make_source_link(last_file, last_start, last_end))

        # Detect changes in comments
        for (parsed, orig_line_no), comment in zip(snippets, updated_comments):
            if comment != lines[orig_line_no]:
                changed = True

    if inject and changed:
        md_path.write_text('\n'.join(output_lines) + '\n')
        print(f"  INJECTED: {md_path.name}")
    elif inject and not changed:
        print(f"  UP-TO-DATE: {md_path.name}")

    return errors


def main():
    inject = '--inject' in sys.argv
    check = '--check' in sys.argv or not inject
    files = [a for a in sys.argv[1:] if not a.startswith('--')]

    if not files:
        files = ['README.md']

    root = Path(__file__).resolve().parent.parent
    total_errors = 0

    for f in files:
        md_path = root / f
        if not md_path.exists():
            print(f"  SKIP: {f} not found")
            continue
        total_errors += process_markdown(md_path, root, inject)

    if total_errors > 0:
        print(f"\n  {total_errors} snippet(s) out of sync!")
        if check:
            sys.exit(1)
    elif not inject:
        print("  All snippets verified.")


if __name__ == '__main__':
    main()
