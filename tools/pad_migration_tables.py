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


def count_lines(text: str) -> int:
    """Count content lines in a code block body (between ``` markers).

    Trailing newline before ``` is structural, not a content line.
    But blank lines before that ARE content lines (for visual padding).
    We count: strip one trailing newline (the one before ```), then
    count what remains.
    """
    # Remove the single trailing newline that separates content from ```
    if text.endswith('\n'):
        text = text[:-1]
    if not text:
        return 0
    return text.count('\n') + 1


def pad_to(text: str, target_lines: int) -> str:
    """Pad a code block body with trailing blank lines to reach target_lines."""
    # Normalize: strip to content lines only
    stripped = text.rstrip('\n')
    current = count_lines(stripped + '\n')
    padding = max(0, target_lines - current)
    # Each padding "line" is just a newline. Final newline closes the block.
    return stripped + '\n' * padding + '\n'


def check(content: str) -> list[str]:
    """Return list of error messages for mismatched pairs."""
    errors = []
    for i, (ls, le, rs, re_) in enumerate(find_table_pairs(content)):
        left_lines = count_lines(content[ls:le])
        right_lines = count_lines(content[rs:re_])
        if left_lines != right_lines:
            errors.append(
                f"Table pair {i+1}: left={left_lines} right={right_lines}"
            )
    return errors


def fix(content: str) -> str:
    """Fix all mismatched pairs by padding the shorter side."""
    # Repeat until stable — each pass re-finds pairs with current offsets
    for _ in range(10):
        pairs = find_table_pairs(content)
        changed = False
        # Process in reverse so later offsets aren't affected
        for ls, le, rs, re_ in reversed(pairs):
            left_text = content[ls:le]
            right_text = content[rs:re_]
            left_lines = count_lines(left_text)
            right_lines = count_lines(right_text)
            if left_lines == right_lines:
                continue
            target = max(left_lines, right_lines)

            new_right = pad_to(right_text, target)
            content = content[:rs] + new_right + content[re_:]

            new_left = pad_to(left_text, target)
            content = content[:ls] + new_left + content[le:]
            changed = True

        if not changed:
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
