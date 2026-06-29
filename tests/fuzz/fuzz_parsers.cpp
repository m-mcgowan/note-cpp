/// @file fuzz_parsers.cpp
/// Fuzz harness for the JSON SAX lexer and the JSONB streaming parser.
///
/// Portable by design: builds with any C++ compiler under ASan/UBSan, so it
/// runs in the normal toolchains (Apple clang ships no libFuzzer). It also
/// exposes the standard libFuzzer entry point (LLVMFuzzerTestOneInput) so a
/// fuzzer-capable clang (CI / brew llvm) can drive coverage-guided fuzzing.
///
/// The portable main() does deterministic random + mutation fuzzing and
/// replays any corpus files passed as args, so a crash that is found can be
/// committed under tests/fuzz/corpus/ as a permanent regression input.

#include <note/json_sax.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/lexer/parse.hpp>
#include <note/lexer/sax_adapter.hpp>
#include <note/jsonb.hpp>

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace {

// Sink that ignores every SAX event (all JsonSink methods default to no-op).
struct NullSink : note::JsonSink {};

// Feeds a byte span to the streaming parser's read callback.
struct SpanReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    note::Result<size_t> operator()(uint8_t* buf, size_t max, uint32_t) {
        if (pos >= size) return size_t(0);
        size_t n = (max < size - pos) ? max : size - pos;
        memcpy(buf, data + pos, n);
        pos += n;
        return n;
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Target 1: JSON SAX lexer over the raw bytes.
    {
        NullSink sink;
        note::sax_lex(reinterpret_cast<const char*>(data), size, sink);
    }
    // Target 2: JSONB streaming parser over the raw bytes.
    {
        NullSink sink;
        SpanReader reader{data, size};
        char storage[512];
        note::SaxStreamBuf buf(storage);
        auto dispatch = note::make_sax_dispatch(sink);
        note::jsonb_parse_streaming(reader, 1000, buf, dispatch);
    }
    return 0;
}

#ifndef NOTE_FUZZ_LIBFUZZER
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
// Deterministic xorshift RNG — reproducible runs, no <random> dependency.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    uint32_t below(uint32_t n) { return n ? uint32_t(next() % n) : 0; }
};

// A few seed inputs the mutator starts from (valid-ish JSON + JSONB framing).
const char* kSeeds[] = {
    "{\"req\":\"card.version\"}",
    "{\"a\":1,\"b\":[true,null,3.14,\"x\"],\"c\":{\"d\":-5}}",
    "[]", "{}", "\"\\u00e9\\n\\t\"", "1e999", "{\"k\":",
    "{:\x01\x80:}",  // JSONB-ish framing
};

void mutate(std::vector<uint8_t>& v, Rng& rng) {
    if (v.empty()) { v.push_back(uint8_t(rng.next())); return; }
    int ops = 1 + rng.below(8);
    for (int i = 0; i < ops; ++i) {
        switch (rng.below(4)) {
            case 0: v[rng.below(uint32_t(v.size()))] = uint8_t(rng.next()); break;       // flip byte
            case 1: v.insert(v.begin() + rng.below(uint32_t(v.size()) + 1), uint8_t(rng.next())); break; // insert
            case 2: if (v.size() > 1) v.erase(v.begin() + rng.below(uint32_t(v.size()))); break; // delete
            case 3: v[rng.below(uint32_t(v.size()))] ^= uint8_t(1u << rng.below(8)); break; // bit flip
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    // Replay any corpus files passed as args (regression mode).
    bool replayed = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        FILE* f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[i]); continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> b(n > 0 ? size_t(n) : 0);
        if (!b.empty() && fread(b.data(), 1, b.size(), f) != b.size()) { fclose(f); continue; }
        fclose(f);
        LLVMFuzzerTestOneInput(b.data(), b.size());
        replayed = true;
    }
    if (replayed) { printf("corpus replay clean\n"); return 0; }

    // Bounded deterministic random + mutation fuzzing.
    unsigned long iters = 200000;
    if (const char* e = getenv("FUZZ_ITERS")) iters = strtoul(e, nullptr, 10);
    uint64_t seed = 1; if (const char* e = getenv("FUZZ_SEED")) seed = strtoull(e, nullptr, 10);
    Rng rng(seed);
    size_t nseeds = sizeof(kSeeds) / sizeof(kSeeds[0]);
    for (unsigned long it = 0; it < iters; ++it) {
        const char* seedp = kSeeds[rng.below(uint32_t(nseeds))];
        std::vector<uint8_t> v(seedp, seedp + strlen(seedp));
        int rounds = 1 + rng.below(6);
        for (int r = 0; r < rounds; ++r) mutate(v, rng);
        LLVMFuzzerTestOneInput(v.data(), v.size());
    }
    printf("fuzz clean: %lu iterations, seed=%llu\n", iters, (unsigned long long)seed);
    return 0;
}
#endif
