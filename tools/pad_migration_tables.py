#!/usr/bin/env python3
"""Pad code blocks in migration guide tables so left and right have equal line counts.

Usage:
    python3 tools/pad_migration_tables.py [--check]

Without --check: fixes the file in place.
With --check: exits non-zero if any pair is mismatched (for CI/pre-commit).
"""

import re
import sys
from pathlib import Path

MIGRATION_DOC = Path(__file__).parent.parent / "docs" / "migration-from-note-arduino.md"


def find_table_pairs(content: str) -> list[tuple[int, int, int, int]]:
    """Find pairs of code blocks inside <table> elements.

    Returns list of (left_start, left_end, right_start, right_end) where
    start/end are positions of the content between ``` markers.
    """
    pairs = []
    # Find <table>...</table> blocks
    for table_m in re.finditer(r'<table>(.*?)</table>', content, re.DOTALL):
        table = table_m.group(1)
        table_start = table_m.start(1)

        # Find code blocks within this table
        blocks = list(re.finditer(r'```\w*\n(.*?)```', table, re.DOTALL))
        for i in range(0, len(blocks) - 1, 2):
            left = blocks[i]
            right = blocks[i + 1]
            pairs.append((
                table_start + left.start(1), table_start + left.end(1),
                table_start + right.start(1), table_start + right.end(1),
            ))
    return pairs


def visible_lines(text: str) -> list[str]:
    """Split text between ``` markers into the lines a renderer shows.

    The text always ends with \\n (before the closing ```).
    A code block with content "a\\nb\\n" renders as 2 lines: ["a", "b"].
    A code block with "a\\nb\\n\\n" renders as 3 lines: ["a", "b", ""].
    """
    # The final \n is structural (before ```), not a visible line.
    if text.endswith('\n'):
        text = text[:-1]
    if not text:
        return []
    return text.split('\n')


def content_line_count(text: str) -> int:
    """Count non-trailing-blank visible lines."""
    lines = visible_lines(text)
    while lines and lines[-1].strip() == '':
        lines.pop()
    return len(lines)


def pad_to(text: str, target: int) -> str:
    """Normalize a code block to exactly `target` visible lines.

    Strips trailing blank lines, pads with blanks to reach target.
    The longer side gets zero padding (no trailing blanks).
    """
    lines = visible_lines(text)
    # Strip trailing blanks
    while lines and lines[-1].strip() == '':
        lines.pop()
    # Pad to target
    while len(lines) < target:
        lines.append('')
    return '\n'.join(lines) + '\n'


def check(content: str) -> list[str]:
    """Return list of error messages for mismatched pairs.

    Both sides must have the same number of visible lines, and the
    longer side (by content) must have no trailing blank lines.
    """
    errors = []
    for i, (ls, le, rs, re_) in enumerate(find_table_pairs(content)):
        left_text = content[ls:le]
        right_text = content[rs:re_]
        left_vis = len(visible_lines(left_text))
        right_vis = len(visible_lines(right_text))
        left_c = content_line_count(left_text)
        right_c = content_line_count(right_text)
        target = max(left_c, right_c)

        if left_vis != target or right_vis != target:
            errors.append(
                f"Table pair {i+1}: left={left_vis}({left_c} content) "
                f"right={right_vis}({right_c} content) target={target}"
            )
    return errors


def fix(content: str) -> str:
    """Fix all mismatched pairs by padding the shorter side."""
    # Fix one pair at a time, re-finding all pairs each iteration.
    # This avoids stale offsets after modifying content.
    for _ in range(50):  # safety limit
        pairs = find_table_pairs(content)
        fixed_any = False
        for ls, le, rs, re_ in pairs:
            left_text = content[ls:le]
            right_text = content[rs:re_]
            left_n = content_line_count(left_text)
            right_n = content_line_count(right_text)
            target = max(left_n, right_n)

            new_left = pad_to(left_text, target)
            new_right = pad_to(right_text, target)

            # Skip if both already correct
            if new_left == left_text and new_right == right_text:
                continue

            # Replace right first (later in file) so left offsets stay valid
            content = content[:rs] + new_right + content[re_:]
            content = content[:ls] + new_left + content[le:]

            fixed_any = True
            break  # restart — offsets are now stale for remaining pairs

        if not fixed_any:
            break
    return content


def main():
    check_only = "--check" in sys.argv

    if not MIGRATION_DOC.exists():
        print(f"Migration doc not found: {MIGRATION_DOC}")
        sys.exit(1)

    content = MIGRATION_DOC.read_text()
    errors = check(content)

    if check_only:
        if errors:
            print("Migration guide table pairs have mismatched line counts:")
            for e in errors:
                print(f"  {e}")
            sys.exit(1)
        else:
            print("Migration guide tables: all pairs aligned.")
            sys.exit(0)

    if errors:
        fixed = fix(content)
        MIGRATION_DOC.write_text(fixed)
        print(f"Fixed {len(errors)} mismatched table pair(s).")
        # Verify
        verify_errors = check(fixed)
        if verify_errors:
            print("WARNING: some pairs still mismatched after fix:")
            for e in verify_errors:
                print(f"  {e}")
            sys.exit(1)
    else:
        print("All table pairs already aligned.")


if __name__ == "__main__":
    main()
