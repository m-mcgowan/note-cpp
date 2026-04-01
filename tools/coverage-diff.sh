#!/usr/bin/env bash
# Compare coverage between two LCOV tracefiles.
#
# Usage:
#   tools/coverage-diff.sh [before.lcov] [after.lcov]
#
# Defaults to comparing coverage/coverage-before.lcov vs coverage/coverage.lcov.
# Tip: copy coverage.lcov to coverage-before.lcov before making changes,
# then run ci.sh --full and this script to see what improved.

set -euo pipefail

BEFORE="${1:-coverage/coverage-before.lcov}"
AFTER="${2:-coverage/coverage.lcov}"

if [[ ! -f "$BEFORE" ]]; then
    echo "Error: $BEFORE not found."
    echo "Tip: cp coverage/coverage.lcov coverage/coverage-before.lcov"
    echo "     # make changes, run ci.sh --full"
    echo "     tools/coverage-diff.sh"
    exit 1
fi

if [[ ! -f "$AFTER" ]]; then
    echo "Error: $AFTER not found. Run './ci.sh --full' first."
    exit 1
fi

parse_summary() {
    local file="$1"
    lcov --summary "$file" --rc branch_coverage=1 2>&1 | awk '
        /lines/     { gsub(/%/, ""); print "lines " $2 }
        /functions/ { gsub(/%/, ""); print "funcs " $2 }
        /branches/  { gsub(/%/, ""); print "branches " $2 }
    '
}

echo "=== Coverage comparison ==="
echo "  Before: $BEFORE"
echo "  After:  $AFTER"
echo

# Parse both
declare -A BEFORE_PCT AFTER_PCT
while read -r key val; do BEFORE_PCT[$key]=$val; done < <(parse_summary "$BEFORE")
while read -r key val; do AFTER_PCT[$key]=$val; done < <(parse_summary "$AFTER")

printf "  %-12s %8s → %8s  %s\n" "Metric" "Before" "After" "Delta"
printf "  %s\n" "$(printf -- '-%.0s' {1..45})"

for metric in lines funcs branches; do
    b="${BEFORE_PCT[$metric]}"
    a="${AFTER_PCT[$metric]}"
    delta=$(python3 -c "print(f'{$a - $b:+.1f}%')")
    printf "  %-12s %7s%% → %7s%%  %s\n" "$metric" "$b" "$a" "$delta"
done

echo
echo "=== Per-file changes ==="

# Generate per-file diff using Python
python3 - "$BEFORE" "$AFTER" <<'PYEOF'
import sys

def parse_file_coverage(path):
    """Returns {filename: (lines_found, lines_hit)}"""
    result = {}
    current_file = None
    lf = lh = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith('SF:'):
                current_file = line[3:]
                lf = lh = 0
            elif line.startswith('LF:'):
                lf = int(line[3:])
            elif line.startswith('LH:'):
                lh = int(line[3:])
            elif line == 'end_of_record' and current_file:
                # Shorten path
                for prefix in ['/Users/mat/e/note-cpp/include/note/', '/Users/mat/e/note-cpp/src/note/']:
                    if current_file.startswith(prefix):
                        current_file = current_file[len(prefix):]
                        break
                result[current_file] = (lf, lh)
                current_file = None
    return result

before = parse_file_coverage(sys.argv[1])
after = parse_file_coverage(sys.argv[2])

all_files = sorted(set(before) | set(after))
changes = []
for f in all_files:
    bf, bh = before.get(f, (0, 0))
    af, ah = after.get(f, (0, 0))
    b_pct = 100.0 * bh / bf if bf else 0
    a_pct = 100.0 * ah / af if af else 0
    delta_lines = (ah - (af - bf)) - bh  # additional lines hit, adjusted for new lines
    delta_pct = a_pct - b_pct
    if abs(delta_pct) > 0.1 or af != bf:
        changes.append((delta_pct, f, b_pct, a_pct, bf, af, bh, ah))

changes.sort()  # worst regressions first

if not changes:
    print("  No per-file changes detected.")
else:
    print(f"  {'File':<40} {'Before':>8} {'After':>8} {'Delta':>8}")
    print(f"  {'-'*65}")
    for delta_pct, f, b_pct, a_pct, bf, af, bh, ah in changes:
        marker = "!!!" if delta_pct < -5 else (">>>" if delta_pct > 5 else "   ")
        new = f" (+{af-bf} lines)" if af > bf else ""
        print(f"  {f:<40} {b_pct:>7.1f}% {a_pct:>7.1f}% {delta_pct:>+7.1f}% {marker}{new}")
PYEOF
