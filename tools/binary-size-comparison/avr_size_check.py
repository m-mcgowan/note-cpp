#!/usr/bin/env python3
"""AVR size regression gate.

Builds the StaticNotecard binary-size envs and compares each against
the checked-in baseline in `avr_baselines.json`. Any growth fails with
a non-zero exit (CI-friendly); shrinks are reported but pass.

Baseline updates are explicit: run with `--update` to overwrite the
JSON with current measurements. The expectation is that intentional
size changes land in the same commit as the baseline bump, so the diff
documents the change.

Usage:
    avr_size_check.py            # build + check
    avr_size_check.py --update   # build + write current sizes to baseline
    avr_size_check.py --no-build # skip pio run; expect .pio/build/* fresh
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
BASELINE_FILE = SCRIPT_DIR / "avr_baselines.json"

# pio run prints lines like:
#   RAM:   [====      ]  38.7% (used 792 bytes from 2048 bytes)
#   Flash: [========  ]  77.9% (used 25112 bytes from 32256 bytes)
SIZE_RE = re.compile(
    r"^(RAM|Flash):\s*\[[^\]]*\]\s*[\d.]+%\s+\(used\s+(\d+)\s+bytes\s+from\s+(\d+)\s+bytes\)",
    re.MULTILINE,
)


def parse_sizes(pio_output: str) -> tuple[int, int]:
    """Extract (flash_bytes, ram_bytes) from `pio run` output."""
    flash = ram = None
    for line in pio_output.splitlines():
        m = SIZE_RE.match(line)
        if not m:
            continue
        kind, used, _total = m.groups()
        if kind == "Flash":
            flash = int(used)
        elif kind == "RAM":
            ram = int(used)
    if flash is None or ram is None:
        raise RuntimeError(
            f"could not parse Flash/RAM sizes from pio output:\n{pio_output}"
        )
    return flash, ram


def build_env(env: str) -> tuple[int, int]:
    """Run `pio run -e <env>` and return (flash, ram)."""
    print(f"  Building {env}...", flush=True)
    proc = subprocess.run(
        ["pio", "run", "-d", str(SCRIPT_DIR), "-e", env],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"pio run -e {env} failed (exit {proc.returncode})")
    return parse_sizes(proc.stdout)


def measure(envs: list[str], skip_build: bool) -> dict[str, dict[str, int]]:
    """Build (or just parse) and return per-env (flash, ram) sizes."""
    if skip_build:
        # Re-run pio in --silent mode to surface the size lines without
        # rebuilding (when sources are unchanged, pio is fast here).
        out: dict[str, dict[str, int]] = {}
        for env in envs:
            proc = subprocess.run(
                ["pio", "run", "-d", str(SCRIPT_DIR), "-e", env, "--silent"],
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode != 0:
                sys.stderr.write(proc.stdout)
                sys.stderr.write(proc.stderr)
                raise SystemExit(f"pio run --silent failed for {env}")
            # `--silent` suppresses everything but errors; for sizes we
            # need a normal run. Fall back if no size lines came out.
            try:
                flash, ram = parse_sizes(proc.stdout)
            except RuntimeError:
                flash, ram = build_env(env)
            out[env] = {"flash": flash, "ram": ram}
        return out
    return {env: dict(zip(("flash", "ram"), build_env(env))) for env in envs}


def report(env: str, baseline: dict, current: dict) -> bool:
    """Print a delta line. Returns True on regression (any growth)."""
    df = current["flash"] - baseline["flash"]
    dr = current["ram"] - baseline["ram"]
    arrow_f = "▲" if df > 0 else ("▼" if df < 0 else "·")
    arrow_r = "▲" if dr > 0 else ("▼" if dr < 0 else "·")
    print(
        f"  {env:24s} flash {current['flash']:>6d} ({arrow_f}{df:+5d})"
        f"   ram {current['ram']:>4d} ({arrow_r}{dr:+4d})"
    )
    return df > 0 or dr > 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--update",
        action="store_true",
        help="Write current sizes to the baseline JSON.",
    )
    ap.add_argument(
        "--no-build",
        action="store_true",
        help="Skip pio run; trust the existing .pio/build/<env>/ artifacts.",
    )
    args = ap.parse_args()

    with BASELINE_FILE.open() as f:
        spec = json.load(f)
    envs = list(spec["envs"].keys())

    print("=== AVR size check ===")
    sizes = measure(envs, args.no_build)

    print()
    print(f"  {'env':24s} {'flash (Δ)':>20s}   {'ram (Δ)':>15s}")
    regressed = []
    for env in envs:
        if report(env, spec["envs"][env], sizes[env]):
            regressed.append(env)

    if args.update:
        for env in envs:
            spec["envs"][env]["flash"] = sizes[env]["flash"]
            spec["envs"][env]["ram"] = sizes[env]["ram"]
        with BASELINE_FILE.open("w") as f:
            json.dump(spec, f, indent=2)
            f.write("\n")
        print()
        print(f"  Updated baseline: {BASELINE_FILE.relative_to(SCRIPT_DIR.parent.parent)}")
        return 0

    if regressed:
        print()
        print(f"FAIL: size regression in {len(regressed)} env(s): {', '.join(regressed)}")
        print(
            "      If this is intentional, run "
            "`tools/binary-size-comparison/avr_size_check.py --update` "
            "and commit the updated baseline alongside the change."
        )
        return 1

    print()
    print("  All AVR envs at or below baseline.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
