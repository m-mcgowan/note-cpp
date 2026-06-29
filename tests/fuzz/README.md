# Parser fuzzing

`fuzz_parsers.cpp` fuzzes the two untrusted-input parsers — the JSON SAX lexer
(`note::sax_lex`) and the JSONB streaming parser (`note::jsonb_parse_streaming`)
— against arbitrary bytes, under AddressSanitizer + UndefinedBehaviorSanitizer.

## Why portable

Apple's clang ships no libFuzzer runtime, so the harness does **not** depend on
`-fsanitize=fuzzer`. It exposes the standard `LLVMFuzzerTestOneInput(data, size)`
entry point (so a fuzzer-capable clang in CI or via `brew install llvm` *can*
drive coverage-guided fuzzing), plus a portable `main()` that:

- **replays a corpus** — any file paths passed as arguments are fed to the
  target once each (regression mode);
- **fuzzes** — with no file arguments, runs a bounded, deterministic
  random + mutation campaign seeded from a handful of valid inputs.

## Running

```bash
# Bounded deterministic campaign (default 200k iterations, seed 1):
clang++ -std=c++20 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I include -o /tmp/fuzz tests/fuzz/fuzz_parsers.cpp
/tmp/fuzz                         # fuzz
FUZZ_ITERS=2000000 FUZZ_SEED=7 /tmp/fuzz
/tmp/fuzz tests/fuzz/corpus/*     # replay corpus only

# Coverage-guided (where libFuzzer is available):
clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined -DNOTE_FUZZ_LIBFUZZER \
    -I include -o /tmp/fuzz tests/fuzz/fuzz_parsers.cpp
/tmp/fuzz -max_total_time=60 tests/fuzz/corpus
```

`./ci.sh --fuzz` builds the harness and runs the corpus replay plus a bounded
campaign across several seeds; it is also part of `./ci.sh --full`.

## Corpus

`corpus/` holds seed inputs and, going forward, any crash-triggering input a
run finds — minimized and committed so it is replayed forever as a regression.
As of this writing no crashing input has been found (2M+ iterations clean).
