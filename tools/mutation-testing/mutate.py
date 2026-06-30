#!/usr/bin/env python3
"""Source-level mutation testing for note-cpp's correctness-critical headers.

Mutation testing measures *test strength*, not test reach. Coverage proves a
line executed; a mutant proves a test would *notice* if that line were wrong.
We deliberately corrupt one operator/literal at a time, rebuild the covering
host tests, and run them:

  - the mutant is KILLED if a test fails (good — an assertion caught it),
  - it SURVIVED if every test still passes (a weak/missing assertion, or an
    equivalent mutant that changes no observable behaviour).

A surviving mutant on a critical path is the signal: the durable fix is a new
permanent assertion added to the real suite. The mutants themselves are
ephemeral — applied to a private overlay copy of the header, never to the tree.

Design (see DESIGN-mutation-testing.md for the full rationale):

  * Mutants live in a per-worker overlay dir that *shadows* the real header via
    a `-I <overlay>` placed before `-I include`. The source tree is never
    touched, so workers need no git worktrees and never contend on files.
  * `doctest_main.o` and `alloc_counter.o` are compiled ONCE and cached; a
    mutation only changes a header, so per mutant we recompile just the covering
    test TU(s) + link against the cached objects + run. ~2s per mutant.
  * Mutation sites come from libclang's AST, so a comparison `<` is mutated but a
    template `Foo<int>` `<` is not, and strings/comments are never touched. Every
    emitted mutant is syntactically valid; the few that fail to type-check
    (e.g. an `==`->`!=` that breaks an overload) are reported as "uncompilable"
    and excluded from the score rather than miscounted as survivors.
  * A process pool runs ~cpu-2 workers; a per-mutant run timeout guards against
    mutations that induce an infinite loop.

This is an on-demand audit (wired as `ci.sh --mutate`), NOT a per-commit gate:
it is minutes long and scores are inherently noisy (equivalent mutants survive
legitimately and need human triage). By default it exits 0 even with survivors;
pass --fail-on-survivors to gate.

Requires: a C++ compiler (default `c++`) and the `clang.cindex` Python bindings
(`pip install libclang`). Self-describes and exits cleanly if either is absent.
"""

from __future__ import annotations

import argparse
import multiprocessing as mp
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# ---------------------------------------------------------------------------
# Targets: each correctness-critical header mapped to the host test TU(s) that
# exercise it. A mutation in `header` rebuilds + runs `tests` (linked together
# into one doctest binary). Verify coverage of a new target with
# `./ci.sh --coverage` before adding it here.
# ---------------------------------------------------------------------------

@dataclass
class Target:
    name: str
    header: str           # path relative to ROOT, must live under include/
    tests: list[str]      # test TU paths relative to ROOT
    poc: bool = False     # part of the fast default set wired into ci.sh --mutate

TARGETS: dict[str, Target] = {t.name: t for t in [
    # PoC pair — smallest + highest stakes (wire correctness + retry safety).
    # NB: `tests` must list EVERY TU that directly asserts the target's
    # behaviour, not just the obvious unit test — a mutant is only "killed" if a
    # *rebuilt* test fails. e.g. cobs_encoded_length is asserted in
    # test_binary_execute.cpp, not test_cobs.cpp; omitting it yields false
    # survivors. Verify the set against host coverage when adding a target.
    Target("cobs",  "include/note/link/cobs.hpp",
           ["tests/test_cobs.cpp", "tests/test_binary_execute.cpp"], poc=True),
    Target("retry", "include/note/retry.hpp",
           ["tests/test_retry.cpp", "tests/test_static_notecard.cpp"], poc=True),
    # Full-pass targets (phase 2). Pre-flight will skip any whose baseline build
    # or test run is not green on this machine, with a clear message.
    Target("json_lexer", "include/note/lexer/json_lexer.hpp",
           ["tests/test_json_lexer.cpp", "tests/test_json_sax.cpp"]),
    Target("jsonb", "include/note/jsonb.hpp",
           ["tests/test_jsonb.cpp", "tests/test_json_sax_streaming.cpp"]),
    Target("dispatch", "include/note/notecard.hpp",
           ["tests/test_notecard.cpp", "tests/test_endpoint_coverage.cpp"]),
]}

# ---------------------------------------------------------------------------
# Mutation operators. Operator-token swaps are looked up by spelling regardless
# of the AST node subkind, so binary `+` and compound `+=` are both covered.
# ---------------------------------------------------------------------------

# Relational / equality / logical / arithmetic operator swaps (the standard
# catalogue). Each maps an operator token to its replacement(s).
OP_MUTATIONS: dict[str, list[str]] = {
    "<":  ["<="], "<=": ["<"],
    ">":  [">="], ">=": [">"],
    "==": ["!="], "!=": ["=="],
    "&&": ["||"], "||": ["&&"],
    "+":  ["-"],  "-":  ["+"],
    "*":  ["/"],  "/":  ["*"],
    "+=": ["-="], "-=": ["+="],
    "*=": ["/="], "/=": ["*="],
}


@dataclass
class Mutant:
    target: str
    header: str          # relative path
    offset: int          # byte offset of the span being replaced
    length: int          # length of the original span
    original: str
    replacement: str
    line: int
    col: int
    op_label: str        # short description for the report

    def key(self):       # for de-duping cursors duplicated by template instantiation
        return (self.offset, self.length, self.replacement)

    def describe(self):
        return (f"{self.header}:{self.line}:{self.col}  "
                f"{self.original!r} -> {self.replacement!r}  ({self.op_label})")


# ---------------------------------------------------------------------------
# libclang setup
# ---------------------------------------------------------------------------

def init_libclang():
    """Import clang.cindex and confirm a usable libclang. Returns the module or
    None (with a printed reason) so callers can self-skip like run_fuzz does."""
    try:
        import clang.cindex as cindex
    except ImportError:
        print("  clang.cindex not installed — skipping mutation testing.")
        print("  Enable it with:  python3 -m pip install libclang")
        return None
    # The pip `libclang` wheel bundles a matching native lib; prefer it. Only
    # fall back to set_library_file if the default load fails.
    try:
        cindex.Index.create()
        return cindex
    except cindex.LibclangError:
        pass
    for cand in ("/usr/local/opt/llvm/lib/libclang.dylib",
                 "/opt/homebrew/opt/llvm/lib/libclang.dylib",
                 "/usr/lib/llvm-18/lib/libclang.so.1",
                 "/usr/lib/x86_64-linux-gnu/libclang-18.so.1"):
        if os.path.exists(cand):
            try:
                cindex.Config.set_library_file(cand)
                cindex.Index.create()
                return cindex
            except cindex.LibclangError:
                cindex.Config.loaded = False  # allow another attempt
    print("  no usable libclang found — skipping mutation testing.")
    return None


def system_includes(cxx: str, std: str) -> list[str]:
    """Derive the compiler's own system include search dirs so libclang can
    resolve <cstddef> etc. Portable across clang/gcc, macOS/Linux."""
    out = subprocess.run([cxx, "-x", "c++", f"-std={std}", "-E", "-v", "-"],
                         input="", capture_output=True, text=True).stderr
    dirs, capturing = [], False
    for ln in out.splitlines():
        if "search starts here" in ln:
            capturing = True
            continue
        if "End of search list" in ln:
            break
        if capturing:
            dirs.append(ln.strip().split(" (")[0])
    return dirs


# ---------------------------------------------------------------------------
# Mutation-site discovery via the AST
# ---------------------------------------------------------------------------

def _operator_token(cindex, cursor):
    """The operator token of a (compound-)assignment / binary / unary cursor.
    Found positionally: the first punctuation token at or after the end of the
    left operand. For unary nodes (one child) it is the first punctuation token."""
    children = list(cursor.get_children())
    toks = list(cursor.get_tokens())
    if not toks:
        return None
    if len(children) >= 2:
        lhs_end = children[0].extent.end.offset
        for t in toks:
            if (t.extent.start.offset >= lhs_end
                    and t.kind == cindex.TokenKind.PUNCTUATION
                    and t.spelling in OP_MUTATIONS):
                return t
        return None
    # unary
    t = toks[0]
    return t if t.kind == cindex.TokenKind.PUNCTUATION else None


def _mutate_int_literal(spelling: str) -> list[str]:
    """Boundary mutations for an integer literal token (preserving base+suffix):
    n -> n+1 and n -> n-1 (which also yields 0<->1)."""
    s = spelling
    suffix = ""
    while s and s[-1] in "uUlL":
        suffix = s[-1] + suffix
        s = s[:-1]
    try:
        if s.lower().startswith("0x"):
            base, val, fmt = 16, int(s, 16), lambda v: "0x%X" % v
        elif s.lower().startswith("0b"):
            base, val, fmt = 2, int(s, 2), lambda v: "0b" + format(v, "b")
        elif len(s) > 1 and s.startswith("0"):
            base, val, fmt = 8, int(s, 8), lambda v: "0%o" % v
        else:
            base, val, fmt = 10, int(s, 10), str
    except ValueError:
        return []
    out = []
    for nv in (val + 1, val - 1):
        if nv < 0:
            continue
        out.append(fmt(nv) + suffix)
    return out


def discover_mutants(cindex, target: Target, parse_args: list[str]) -> list[Mutant]:
    header_abs = os.path.join(ROOT, target.header)
    src = open(header_abs, "rb").read()
    idx = cindex.Index.create()
    tu = idx.parse(header_abs, args=parse_args)
    errs = [d for d in tu.diagnostics if d.severity >= cindex.Diagnostic.Error]
    if errs:
        print(f"  WARNING: {len(errs)} parse error(s) in {target.header}; "
              f"site discovery may be incomplete. First: {errs[0].spelling}")

    BO = cindex.CursorKind.BINARY_OPERATOR
    CAO = getattr(cindex.CursorKind, "COMPOUND_ASSIGNMENT_OPERATOR", None)
    UO = cindex.CursorKind.UNARY_OPERATOR
    IL = cindex.CursorKind.INTEGER_LITERAL
    BL = cindex.CursorKind.CXX_BOOL_LITERAL_EXPR
    CALL = cindex.CursorKind.CALL_EXPR

    mutants: dict[tuple, Mutant] = {}

    def add(offset, length, original, replacement, line, col, label):
        m = Mutant(target.name, target.header, offset, length,
                   original, replacement, line, col, label)
        mutants.setdefault(m.key(), m)

    def in_header(cur):
        f = cur.location.file
        return f and os.path.abspath(f.name) == header_abs

    def walk(cur):
        yield cur
        for ch in cur.get_children():
            yield from walk(ch)

    for cur in walk(tu.cursor):
        if not in_header(cur):
            continue
        kind = cur.kind

        # Operator-token swaps (binary, compound-assignment, unary).
        if kind in (BO, UO) or (CAO is not None and kind == CAO):
            tok = _operator_token(cindex, cur)
            if tok is None:
                continue
            sp = tok.spelling
            off = tok.extent.start.offset
            if kind == UO:
                # Drop a unary logical-not: `!x` -> `x`.
                if sp == "!":
                    add(off, 1, "!", "", tok.location.line, tok.location.column,
                        "drop unary !")
                continue
            for repl in OP_MUTATIONS.get(sp, []):
                add(off, len(sp), sp, repl, tok.location.line, tok.location.column,
                    "operator swap")

        # Integer-literal boundary mutations.
        elif kind == IL:
            toks = [t for t in cur.get_tokens()
                    if t.kind == cindex.TokenKind.LITERAL]
            if not toks:
                continue
            tok = toks[0]
            for repl in _mutate_int_literal(tok.spelling):
                add(tok.extent.start.offset, len(tok.spelling), tok.spelling, repl,
                    tok.location.line, tok.location.column, "literal boundary")

        # Boolean-literal flip.
        elif kind == BL:
            toks = list(cur.get_tokens())
            if not toks:
                continue
            tok = toks[0]
            if tok.spelling in ("true", "false"):
                repl = "false" if tok.spelling == "true" else "true"
                add(tok.extent.start.offset, len(tok.spelling), tok.spelling, repl,
                    tok.location.line, tok.location.column, "bool flip")

        # Statement deletion: a standalone `call(...);` expression statement.
        elif kind == CALL:
            ext = cur.extent
            start, end = ext.start.offset, ext.end.offset
            # Preceding non-whitespace must terminate a statement (so the call
            # is a statement, not part of a larger expression).
            i = start - 1
            while i >= 0 and src[i:i+1].isspace():
                i -= 1
            if i < 0 or src[i:i+1] not in (b";", b"{", b"}"):
                continue
            # Following non-whitespace must be the terminating semicolon.
            j = end
            while j < len(src) and src[j:j+1].isspace():
                j += 1
            if j >= len(src) or src[j:j+1] != b";":
                continue
            add(start, (j + 1) - start, src[start:j+1].decode("utf-8", "replace"),
                "", ext.start.line, ext.start.column, "delete statement")

    return list(mutants.values())


# ---------------------------------------------------------------------------
# Per-mutant build + run (worker)
# ---------------------------------------------------------------------------

CONFIG: dict = {}  # populated in each worker by _init_worker


def _init_worker(config):
    global CONFIG
    CONFIG = config


def build_shadow_include(work: str, header_relpath: str, mutated: bytes) -> str:
    """Build a shadow copy of include/ where every header is a symlink to the
    real file EXCEPT the mutated one, which is a real (mutated) file.

    Compiling against this tree as the SOLE include root makes both angle
    (`<note/link/cobs.hpp>`) and quote (`"link/cobs.hpp"`, resolved relative to
    the including file's dir) includes land on the same shadow path — so the
    mutated header wins uniformly and `#pragma once` still dedupes by identity.
    A flat `-I overlay -I include` would let quote-includes slip past the
    overlay to the real header, double-including and breaking the build."""
    include_root = os.path.join(ROOT, "include")
    overlay = os.path.join(work, "inc")
    parts = header_relpath.split(os.sep)
    src_dir, dst_dir = include_root, overlay
    os.makedirs(dst_dir, exist_ok=True)

    def link_siblings(s_dir, d_dir, skip):
        for entry in os.listdir(s_dir):
            if entry != skip:
                os.symlink(os.path.join(s_dir, entry), os.path.join(d_dir, entry))

    for part in parts[:-1]:           # directory components leading to the header
        link_siblings(src_dir, dst_dir, part)
        src_dir = os.path.join(src_dir, part)
        dst_dir = os.path.join(dst_dir, part)
        os.makedirs(dst_dir, exist_ok=True)
    link_siblings(src_dir, dst_dir, parts[-1])
    with open(os.path.join(dst_dir, parts[-1]), "wb") as f:
        f.write(mutated)
    return overlay


def _run_mutant(mutant: Mutant):
    cxx = CONFIG["cxx"]
    std = CONFIG["std"]
    incs = CONFIG["sys_inc_flags"]
    timeout = CONFIG["timeout"]
    cached = CONFIG["cached_objs"]
    tests = CONFIG["target_tests"][mutant.target]

    rel_under_include = os.path.relpath(os.path.join(ROOT, mutant.header),
                                        os.path.join(ROOT, "include"))
    work = tempfile.mkdtemp(prefix="mut-")
    try:
        # Build a shadow include/ tree carrying the one mutated header.
        src = open(os.path.join(ROOT, mutant.header), "rb").read()
        mutated = (src[:mutant.offset]
                   + mutant.replacement.encode()
                   + src[mutant.offset + mutant.length:])
        overlay = build_shadow_include(work, rel_under_include, mutated)

        # Build + run each covering test TU as its own binary, in listed order,
        # stopping at the first failure. List the fastest-compiling unit TU first
        # so the common case (killed by it) never pays the heavier TUs' compile.
        for i, t in enumerate(tests):
            obj = os.path.join(work, f"t{i}.o")
            cc = [cxx, f"-std={std}", "-w", "-pthread",
                  "-I", overlay,
                  "-I", os.path.join(ROOT, "tests"),
                  *incs,
                  "-c", os.path.join(ROOT, t), "-o", obj]
            if subprocess.run(cc, capture_output=True).returncode != 0:
                return ("uncompilable", mutant, None)
            binp = os.path.join(work, f"bin{i}")
            if subprocess.run([cxx, "-pthread", *cached, obj, "-o", binp],
                              capture_output=True).returncode != 0:
                return ("uncompilable", mutant, None)
            try:
                r = subprocess.run([binp], capture_output=True, timeout=timeout)
            except subprocess.TimeoutExpired:
                return ("killed", mutant, "timeout")
            if r.returncode != 0:
                return ("killed", mutant, None)
        return ("survived", mutant, None)
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def compile_cached_objects(cxx, std, incs, build_dir):
    os.makedirs(build_dir, exist_ok=True)
    objs = {
        "doctest_main": ("tests/doctest_main.cpp", os.path.join(build_dir, "doctest_main.o")),
        "alloc_counter": ("tests/common/alloc_counter.cpp", os.path.join(build_dir, "alloc_counter.o")),
    }
    out = []
    for _, (src, obj) in objs.items():
        cmd = [cxx, f"-std={std}", "-w", "-pthread",
               "-I", os.path.join(ROOT, "include"),
               "-I", os.path.join(ROOT, "tests"),
               *incs, "-c", os.path.join(ROOT, src), "-o", obj]
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr.decode("utf-8", "replace"))
            raise SystemExit(f"failed to compile cached object {src}")
        out.append(obj)
    return out


def baseline_ok(cxx, std, incs, cached, target: Target, timeout) -> tuple[bool, str]:
    """Build + run the unmutated tests for a target (sanity gate)."""
    work = tempfile.mkdtemp(prefix="mut-base-")
    try:
        objs = []
        for i, t in enumerate(target.tests):
            obj = os.path.join(work, f"t{i}.o")
            cmd = [cxx, f"-std={std}", "-w", "-pthread",
                   "-I", os.path.join(ROOT, "include"),
                   "-I", os.path.join(ROOT, "tests"),
                   *incs, "-c", os.path.join(ROOT, t), "-o", obj]
            r = subprocess.run(cmd, capture_output=True)
            if r.returncode != 0:
                return False, f"compile failed: {t}\n" + r.stderr.decode("utf-8", "replace")[-800:]
            objs.append(obj)
        binp = os.path.join(work, "bin")
        r = subprocess.run([cxx, "-pthread", *cached, *objs, "-o", binp], capture_output=True)
        if r.returncode != 0:
            return False, "link failed\n" + r.stderr.decode("utf-8", "replace")[-800:]
        r = subprocess.run([binp], capture_output=True, timeout=timeout)
        if r.returncode != 0:
            return False, "tests fail unmutated\n" + r.stdout.decode("utf-8", "replace")[-800:]
        return True, ""
    except subprocess.TimeoutExpired:
        return False, "baseline run timed out"
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Mutation testing for note-cpp critical headers.")
    ap.add_argument("--target", action="append", default=[],
                    help="target name to mutate (repeatable). Default: PoC set (cobs, retry).")
    ap.add_argument("--all", action="store_true", help="mutate every configured target.")
    ap.add_argument("--list-targets", action="store_true")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--timeout", type=float, default=10.0, help="per-mutant run timeout (s).")
    ap.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--std", default="c++20")
    ap.add_argument("--fail-on-survivors", action="store_true",
                    help="exit non-zero if any mutant survives (off by default; survivors need triage).")
    args = ap.parse_args()

    if args.list_targets:
        for t in TARGETS.values():
            tag = " (poc)" if t.poc else ""
            print(f"  {t.name}{tag}: {t.header}  <- {', '.join(t.tests)}")
        return 0

    cxx = args.compiler
    if not shutil.which(cxx):
        print(f"  compiler {cxx!r} not found — skipping mutation testing.")
        return 0
    cindex = init_libclang()
    if cindex is None:
        return 0

    if args.all:
        selected = list(TARGETS.values())
    elif args.target:
        try:
            selected = [TARGETS[n] for n in args.target]
        except KeyError as e:
            raise SystemExit(f"unknown target {e}; see --list-targets")
    else:
        selected = [t for t in TARGETS.values() if t.poc]

    incs = system_includes(cxx, args.std)
    sys_inc_flags = []
    for d in incs:
        sys_inc_flags += ["-isystem", d]
    parse_args = ["-x", "c++", f"-std={args.std}",
                  "-I", os.path.join(ROOT, "include"),
                  "-I", os.path.join(ROOT, "tests"), *sys_inc_flags]

    print(f"Mutation testing  (compiler={cxx}, std={args.std}, jobs={args.jobs})")
    build_dir = os.path.join(tempfile.gettempdir(), "note-cpp-mutate")
    cached = compile_cached_objects(cxx, args.std, sys_inc_flags, build_dir)

    config = {
        "cxx": cxx, "std": args.std, "sys_inc_flags": sys_inc_flags,
        "timeout": args.timeout, "cached_objs": cached,
        "target_tests": {t.name: t.tests for t in selected},
    }

    overall = {"killed": 0, "survived": 0, "uncompilable": 0}
    all_survivors: list[Mutant] = []
    failed_baselines = []

    for target in selected:
        print(f"\n=== {target.name} ===")
        print(f"  header: {target.header}")
        ok, why = baseline_ok(cxx, args.std, sys_inc_flags, cached, target, args.timeout)
        if not ok:
            print(f"  SKIP — baseline not green:\n    {why.strip()[:400]}")
            failed_baselines.append(target.name)
            continue

        mutants = discover_mutants(cindex, target, parse_args)
        print(f"  baseline: PASS    mutants: {len(mutants)}")
        if not mutants:
            continue

        results = {"killed": 0, "survived": 0, "uncompilable": 0}
        survivors: list[Mutant] = []
        tty = sys.stdout.isatty()
        with mp.Pool(args.jobs, initializer=_init_worker, initargs=(config,)) as pool:
            done = 0
            for status, mutant, note in pool.imap_unordered(_run_mutant, mutants):
                results[status] += 1
                if status == "survived":
                    survivors.append(mutant)
                done += 1
                if tty:
                    print(f"\r  progress: {done}/{len(mutants)}  "
                          f"killed={results['killed']} survived={results['survived']} "
                          f"uncompilable={results['uncompilable']}", end="", flush=True)
        if tty:
            print()

        viable = results["killed"] + results["survived"]
        score = (100.0 * results["killed"] / viable) if viable else 100.0
        print(f"  score: {score:.1f}%   "
              f"(killed {results['killed']} / viable {viable}; "
              f"{results['uncompilable']} uncompilable excluded)")
        if survivors:
            print(f"  SURVIVORS ({len(survivors)}):")
            for m in sorted(survivors, key=lambda x: x.offset):
                print(f"    {m.describe()}")
        for k in overall:
            overall[k] += results[k]
        all_survivors += survivors

    viable = overall["killed"] + overall["survived"]
    score = (100.0 * overall["killed"] / viable) if viable else 100.0
    print("\n" + "=" * 70)
    print(f"TOTAL  score {score:.1f}%   killed {overall['killed']} / viable {viable}"
          f"   ({overall['uncompilable']} uncompilable excluded, "
          f"{overall['survived']} survived)")
    if failed_baselines:
        print(f"  baselines not green (skipped): {', '.join(failed_baselines)}")
    if all_survivors:
        print("  Triage each survivor: add a permanent assertion to the covering")
        print("  test, or confirm it is an equivalent mutant (no behaviour change).")

    if failed_baselines:
        return 2  # a configured target couldn't be measured — that's a real error
    if all_survivors and args.fail_on_survivors:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
