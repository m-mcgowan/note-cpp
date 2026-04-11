# Test Coverage

## Running coverage

```bash
./ci.sh --coverage
```

Generates `coverage/html/index.html` and prints a summary including lines, functions, and branches.

## Expected results

| Metric | Coverage |
|--------|----------|
| Lines | ~98.5% |
| Functions | ~99.9% |
| Branches | ~98.9% |

## Branch coverage and exception paths

GCC instruments C++ exception-unwinding paths as branches even when code is
`noexcept` or never throws. These appear as `block=e0` branches in the tracefile
and are structurally unreachable in normal testing — they represent stack cleanup
paths, not program logic.

`ci.sh` passes `--rc no_exception_branch=1` to `lcov --capture` to strip these.
Without this flag, branch coverage appears as ~77% due to ~914 unreachable
exception branches across the header-only codebase. With it, the remaining ~36
missed branches are real program logic gaps.

## Why GCC is required for accurate results

`./ci.sh --coverage` prefers GCC (`g++-13` / `g++-14`) over clang. The two toolchains
differ significantly in how they handle `consteval` functions:

| Toolchain | `consteval` functions | Result |
|-----------|----------------------|--------|
| GCC `--coverage` (gcov) | Marked `-` (non-executable) | Correct — excluded from metrics |
| Clang `-fprofile-instr-generate` | Instrumented but never called | **False positives** — appear as uncovered |

This project uses `static consteval` member functions for compile-time string
validation in every API endpoint (e.g. `validatedMode()`). With clang these show
as uncovered, inflating the function miss count by roughly 30 functions (~10–12%
of the function total).

## Why `lcov --capture` (not genhtml) handles LCOV_EXCL correctly

The lcov file format tracks line coverage (`DA:` records) and function coverage
(`FN:`/`FNDA:` records) separately. `genhtml` applies `LCOV_EXCL_START/STOP`
source annotations to `DA:` records only — function entries are unaffected, so
uncovered functions remain in the function count even when their lines are excluded.

`lcov --capture` applies `LCOV_EXCL` markers at collection time, removing both
line and function entries. This is why the GCC pipeline (which uses `lcov --capture`)
gives clean function coverage numbers, while a clang+genhtml pipeline does not.

## Dependencies

### GCC

**macOS (Homebrew):**
```bash
brew install gcc
```

**Ubuntu / GitHub Actions:**
```bash
sudo apt-get install g++-13
```

The script will find `g++-14` or `g++-13` automatically. If neither is present it
falls back to clang with a warning.

### lcov 2.x

lcov 1.x has an internal `.gcno` format parser that cannot read the format produced
by GCC 13+, causing `"Overlong record at end of file!"` errors and an empty tracefile.
**lcov 2.x is required.**

**macOS (Homebrew):**
```bash
brew install lcov        # fresh install
brew upgrade lcov        # or upgrade from 1.x
```

**Ubuntu 24.04+:**
```bash
sudo apt-get install lcov   # ships lcov 2.x
```

**Ubuntu 22.04 (GitHub Actions `ubuntu-latest` until mid-2025):**
```bash
# apt ships lcov 1.14; install from the upstream PPA instead:
pip install lcov-python  # or grab the release tarball from github.com/linux-test-project/lcov
```

The script exits with an error if lcov < 2.0 is detected.
