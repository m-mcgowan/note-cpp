#!/usr/bin/env python3
"""Analyze coverage gaps from an LCOV tracefile.

Usage:
    tools/coverage-gaps.py [coverage/coverage.lcov]

Reports:
    1. Per-file summary sorted by uncovered lines (biggest gaps first)
    2. Specific uncovered line ranges per file (actionable)
    3. Uncovered function names
    4. Gap-to-threshold: how many more lines/functions/branches needed
"""

import sys
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class FileCoverage:
    path: str
    lines_found: int = 0
    lines_hit: int = 0
    funcs_found: int = 0
    funcs_hit: int = 0
    branches_found: int = 0
    branches_hit: int = 0
    uncovered_lines: list = field(default_factory=list)
    uncovered_functions: list = field(default_factory=list)

    @property
    def lines_missed(self): return self.lines_found - self.lines_hit
    @property
    def funcs_missed(self): return self.funcs_found - self.funcs_hit
    @property
    def branches_missed(self): return self.branches_found - self.branches_hit
    @property
    def line_pct(self): return 100.0 * self.lines_hit / self.lines_found if self.lines_found else 100.0
    @property
    def func_pct(self): return 100.0 * self.funcs_hit / self.funcs_found if self.funcs_found else 100.0
    @property
    def branch_pct(self): return 100.0 * self.branches_hit / self.branches_found if self.branches_found else 100.0

    @property
    def short_path(self):
        # Strip common prefix
        for prefix in ['/Users/mat/e/note-cpp/include/note/', '/Users/mat/e/note-cpp/src/note/']:
            if self.path.startswith(prefix):
                return self.path[len(prefix):]
        return self.path


def parse_lcov(path: str) -> list[FileCoverage]:
    """Parse LCOV tracefile into per-file coverage data."""
    files = []
    current: Optional[FileCoverage] = None
    fn_names: dict[int, str] = {}  # line -> name mapping for current file

    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith('SF:'):
                current = FileCoverage(path=line[3:])
                fn_names = {}
            elif line == 'end_of_record' and current:
                files.append(current)
                current = None
            elif current is None:
                continue
            elif line.startswith('FN:'):
                # FN:line,name (lcov 1.x)
                parts = line[3:].split(',', 1)
                if len(parts) == 2:
                    fn_names[int(parts[0])] = parts[1]
            elif line.startswith('FNDA:'):
                # FNDA:count,name (lcov 1.x)
                parts = line[5:].split(',', 1)
                if len(parts) == 2 and int(parts[0]) == 0:
                    current.uncovered_functions.append(parts[1])
            elif line.startswith('FNA:'):
                # FNA:index,count,name (lcov 2.x)
                parts = line[4:].split(',', 2)
                if len(parts) == 3 and int(parts[1]) == 0:
                    current.uncovered_functions.append(parts[2])
            elif line.startswith('FNF:'):
                current.funcs_found = int(line[4:])
            elif line.startswith('FNH:'):
                current.funcs_hit = int(line[4:])
            elif line.startswith('DA:'):
                parts = line[3:].split(',')
                if len(parts) >= 2 and int(parts[1]) == 0:
                    current.uncovered_lines.append(int(parts[0]))
            elif line.startswith('LF:'):
                current.lines_found = int(line[3:])
            elif line.startswith('LH:'):
                current.lines_hit = int(line[3:])
            elif line.startswith('BRF:'):
                current.branches_found = int(line[4:])
            elif line.startswith('BRH:'):
                current.branches_hit = int(line[4:])

    return files


def collapse_ranges(lines: list[int]) -> list[str]:
    """Collapse [1,2,3,7,8,12] into ['1-3', '7-8', '12']."""
    if not lines:
        return []
    ranges = []
    start = prev = lines[0]
    for n in lines[1:]:
        if n == prev + 1:
            prev = n
        else:
            ranges.append(f"{start}-{prev}" if start != prev else str(start))
            start = prev = n
    ranges.append(f"{start}-{prev}" if start != prev else str(start))
    return ranges


def demangle_name(name: str) -> str:
    """Simplify C++ mangled/demangled names for readability."""
    import subprocess
    try:
        result = subprocess.run(['c++filt', name], capture_output=True, text=True, timeout=2)
        if result.returncode == 0:
            name = result.stdout.strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    # Remove common namespace noise
    name = name.replace('std::basic_string_view<char, std::char_traits<char> >', 'string_view')
    name = re.sub(r'note::(api::)?', '', name)
    # Remove template noise from test lambda names
    name = re.sub(r'C_A_T_C_H_T_E_S_T_\d+', 'TEST', name)
    # Truncate very long names
    if len(name) > 100:
        name = name[:97] + '...'
    return name


def main():
    lcov_path = sys.argv[1] if len(sys.argv) > 1 else 'coverage/coverage.lcov'

    if not Path(lcov_path).exists():
        print(f"Error: {lcov_path} not found. Run './ci.sh --full' first.")
        sys.exit(1)

    files = parse_lcov(lcov_path)

    # Totals
    total_lf = sum(f.lines_found for f in files)
    total_lh = sum(f.lines_hit for f in files)
    total_ff = sum(f.funcs_found for f in files)
    total_fh = sum(f.funcs_hit for f in files)
    total_bf = sum(f.branches_found for f in files)
    total_bh = sum(f.branches_hit for f in files)

    line_pct = 100.0 * total_lh / total_lf if total_lf else 0
    func_pct = 100.0 * total_fh / total_ff if total_ff else 0
    branch_pct = 100.0 * total_bh / total_bf if total_bf else 0

    # Thresholds
    MIN_LINE, MIN_FUNC, MIN_BRANCH = 90, 90, 85

    print("=" * 70)
    print("COVERAGE GAP ANALYSIS")
    print("=" * 70)

    # Threshold gap
    lines_needed = max(0, int(MIN_LINE / 100.0 * total_lf) - total_lh + 1)
    funcs_needed = max(0, int(MIN_FUNC / 100.0 * total_ff) - total_fh + 1)
    branches_needed = max(0, int(MIN_BRANCH / 100.0 * total_bf) - total_bh + 1)

    print(f"\n{'Metric':<12} {'Current':>8} {'Target':>8} {'Gap':>8}  Need to cover")
    print("-" * 65)
    print(f"{'Lines':<12} {line_pct:>7.1f}% {MIN_LINE:>7d}% {line_pct - MIN_LINE:>+7.1f}%  {lines_needed} more lines")
    print(f"{'Functions':<12} {func_pct:>7.1f}% {MIN_FUNC:>7d}% {func_pct - MIN_FUNC:>+7.1f}%  {funcs_needed} more functions")
    print(f"{'Branches':<12} {branch_pct:>7.1f}% {MIN_BRANCH:>7d}% {branch_pct - MIN_BRANCH:>+7.1f}%  {branches_needed} more branches")

    # Note about lcov summary discrepancy
    print(f"\n  (Totals from LCOV file: {total_lf} lines, {total_ff} functions, {total_bf} branches)")
    print(f"  Note: lcov --summary may report different totals due to template")
    print(f"  instantiation counting. This analysis uses per-record FNF/FNH counts.")

    # Per-file gaps, sorted by uncovered lines
    gaps = [f for f in files if f.lines_missed > 0]
    gaps.sort(key=lambda f: f.lines_missed, reverse=True)

    print(f"\n{'=' * 70}")
    print(f"FILES WITH UNCOVERED LINES (sorted by impact)")
    print(f"{'=' * 70}")
    print(f"\n{'File':<40} {'Lines':>6} {'Funcs':>6} {'Branches':>8}  Missed")
    print("-" * 75)

    for f in gaps[:25]:
        print(f"{f.short_path:<40} {f.line_pct:>5.1f}% {f.func_pct:>5.1f}% {f.branch_pct:>7.1f}%  "
              f"L:{f.lines_missed} F:{f.funcs_missed} B:{f.branches_missed}")

    # Detailed uncovered lines for top files
    print(f"\n{'=' * 70}")
    print(f"UNCOVERED LINE RANGES (top files)")
    print(f"{'=' * 70}")

    for f in gaps[:15]:
        if not f.uncovered_lines:
            continue
        ranges = collapse_ranges(sorted(f.uncovered_lines))
        print(f"\n  {f.short_path} ({f.lines_missed} lines, {f.line_pct:.1f}%):")
        # Show ranges in groups of ~5
        for i in range(0, len(ranges), 8):
            print(f"    lines {', '.join(ranges[i:i+8])}")

    # Uncovered functions for top files
    print(f"\n{'=' * 70}")
    print(f"UNCOVERED FUNCTIONS (top files)")
    print(f"{'=' * 70}")

    for f in gaps[:10]:
        if not f.uncovered_functions:
            continue
        print(f"\n  {f.short_path} ({f.funcs_missed} uncovered):")
        for fn in f.uncovered_functions[:10]:
            print(f"    - {demangle_name(fn)}")
        if len(f.uncovered_functions) > 10:
            print(f"    ... and {len(f.uncovered_functions) - 10} more")


if __name__ == '__main__':
    main()
