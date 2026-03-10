#!/usr/bin/env bash
# Code size comparison: note-cpp vs note-c
#
# Measures stripped binary sizes and per-function call-site code sizes
# to quantify the cost (or savings) of type safety.
#
# Usage:
#   ./tools/size_report.sh                     # default: -Os, Apple Clang
#   ./tools/size_report.sh --opt -O2           # specific optimization level
#   ./tools/size_report.sh --note-c ~/e/note-c # path to note-c repo
#   ./tools/size_report.sh --cxx g++-13        # specific C++ compiler
#   ./tools/size_report.sh --cc gcc-13         # specific C compiler
#
# Requires: note-c repo (for comparison), Python 3, C/C++23 compiler
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────
OPT="-Os"
NOTE_C="${NOTE_C:-$HOME/e/note-c}"
CXX="${CXX:-c++}"
CC="${CC:-cc}"
CXXFLAGS="-std=c++2b"
TMPDIR="${TMPDIR:-/tmp}/note-cpp-size-$$"

# ── Parse args ────────────────────────────────────────────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        --opt)     OPT="$2"; shift 2 ;;
        --note-c)  NOTE_C="$2"; shift 2 ;;
        --cxx)     CXX="$2"; shift 2 ;;
        --cc)      CC="$2"; shift 2 ;;
        --help|-h) sed -n '2,/^set/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        *)         echo "Unknown arg: $1"; exit 1 ;;
    esac
done

mkdir -p "$TMPDIR"
trap 'rm -rf "$TMPDIR"' EXIT

echo "C++ compiler: $($CXX --version | head -1)"
echo "C   compiler: $($CC --version | head -1)"
echo "Optimization: $OPT"
echo "note-c path:  $NOTE_C"
echo

# ── Mock backend (shared by all note-cpp benchmarks) ──────────────────────────
cat > "$TMPDIR/mock_backend.hpp" << 'HPP'
#pragma once
#include <note/notecard.hpp>
#include <memory>

struct MockBuilder : note::JsonBuilder {
    MockBuilder& add(note::string_view, bool) override { return *this; }
    MockBuilder& add(note::string_view, int32_t) override { return *this; }
    MockBuilder& add(note::string_view, double) override { return *this; }
    MockBuilder& add(note::string_view, note::string_view) override { return *this; }
    MockBuilder& begin_object(note::string_view) override { return *this; }
    MockBuilder& end_object() override { return *this; }
    MockBuilder& begin_array(note::string_view) override { return *this; }
    MockBuilder& end_array() override { return *this; }
    std::string to_string() override { return "{}"; }
};
struct MockReader : note::JsonReader {
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool d) const override { return d; }
    int32_t get_int(note::string_view, int32_t d) const override { return d; }
    double get_double(note::string_view, double d) const override { return d; }
    note::string_view get_string(note::string_view, note::string_view d) const override { return d; }
    std::unique_ptr<note::JsonReader> get_object(note::string_view) const override { return nullptr; }
    bool has_error() const override { return false; }
    note::string_view get_error() const override { return {}; }
};
struct MockBackend : note::JsonBackend {
    std::unique_ptr<note::JsonBuilder> create_builder() override { return std::make_unique<MockBuilder>(); }
    std::unique_ptr<note::JsonReader> parse_response(note::string_view) override { return std::make_unique<MockReader>(); }
};
HPP

# ── Benchmark: note-cpp binary sizes ──────────────────────────────────────────
cat > "$TMPDIR/cpp_minimal.cpp" << 'EOF'
#include "mock_backend.hpp"
int main() {
    MockBackend backend;
    note::Notecard nc(backend, [](note::string_view, uint32_t) -> note::Result<std::string> {
        return std::string("{}");
    });
    auto r = nc.request("card.version");
    return r ? 0 : 1;
}
EOF

cat > "$TMPDIR/cpp_5api.cpp" << 'EOF'
#include "mock_backend.hpp"
#include <note/api_context.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/hub_sync.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_status.hpp>
#include <note/api/note_add.hpp>
int main() {
    MockBackend backend;
    note::Notecard nc(backend, [](note::string_view, uint32_t) -> note::Result<std::string> {
        return std::string("{}");
    });
    note::Api api(nc);
    api.hubSet().set_product("test").set_mode("periodic").set_outbound(60).execute();
    api.hubSync().execute();
    auto v = api.cardVersion().execute();
    auto s = api.cardStatus().execute();
    api.noteAdd().set_file("data.qo").set_body(R"({\"temp\":22})").execute();
    return v ? 0 : 1;
}
EOF

cat > "$TMPDIR/cpp_allapi.cpp" << 'EOF'
#include "mock_backend.hpp"
#include <note/api_context.hpp>
#include <note/api.hpp>
int main() {
    MockBackend backend;
    note::Notecard nc(backend, [](note::string_view, uint32_t) -> note::Result<std::string> {
        return std::string("{}");
    });
    note::Api api(nc);
    api.hubSet().set_product("test").execute();
    auto v = api.cardVersion().execute();
    return v ? 0 : 1;
}
EOF

cat > "$TMPDIR/cpp_body.cpp" << 'EOF'
#include "mock_backend.hpp"
#include <note/api_context.hpp>
#include <note/body.hpp>
#include <note/api/note_add.hpp>
#include <note/api/note_get.hpp>
#include <note/api/note_template.hpp>
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};
int main() {
    MockBackend backend;
    note::Notecard nc(backend, [](note::string_view, uint32_t) -> note::Result<std::string> {
        return std::string("{}");
    });
    note::Api api(nc);
    api.noteAdd().set_file("s.qo").set_body(R"({"t":1})").execute();
    api.noteAdd().set_file("s.qo").set_body(note::body([](note::JsonBuilder& b) {
        b.add("temp", 22.5);
    })).execute();
    Readings r{.temperature = 22.5f, .humidity = 60};
    api.noteAdd().set_file("s.qo").set_body(r).execute();
    api.noteTemplate().set().set_file("s.qo").set_body(note::template_of<Readings>()).execute();
    auto result = api.noteGet().query().set_file("d.qi").execute();
    if (result) { auto rd = result.body_as<Readings>(); (void)rd.temperature; }
    return 0;
}
EOF

# ── Benchmark: note-c binary sizes ────────────────────────────────────────────
cat > "$TMPDIR/c_5api.c" << 'EOF'
#include "note.h"
#include <stdlib.h>
int main() {
    NoteSetFnDefault(malloc, free, NULL, NULL);
    { J *req = NoteNewRequest("hub.set");
      JAddStringToObject(req, "product", "test");
      JAddStringToObject(req, "mode", "periodic");
      JAddNumberToObject(req, "outbound", 60);
      NoteRequest(req); }
    { J *req = NoteNewRequest("hub.sync"); NoteRequest(req); }
    { J *rsp = NoteRequestResponse(NoteNewRequest("card.version"));
      if (rsp) { const char *v = JGetString(rsp, "version");
                  const char *d = JGetString(rsp, "device");
                  (void)v; (void)d; NoteDeleteResponse(rsp); } }
    { J *rsp = NoteRequestResponse(NoteNewRequest("card.status"));
      if (rsp) { bool u = JGetBool(rsp, "usb");
                  int s = JGetInt(rsp, "storage");
                  (void)u; (void)s; NoteDeleteResponse(rsp); } }
    { J *req = NoteNewRequest("note.add");
      JAddStringToObject(req, "file", "data.qo");
      J *body = JAddObjectToObject(req, "body");
      JAddNumberToObject(body, "temp", 22);
      NoteRequest(req); }
    return 0;
}
EOF

cat > "$TMPDIR/c_body.c" << 'EOF'
#include "note.h"
#include <stdlib.h>
int main() {
    NoteSetFnDefault(malloc, free, NULL, NULL);
    { J *req = NoteNewRequest("note.add");
      JAddStringToObject(req, "file", "s.qo");
      J *body = JAddObjectToObject(req, "body");
      JAddNumberToObject(body, "temperature", 22.5);
      JAddNumberToObject(body, "humidity", 60);
      NoteRequest(req); }
    { J *req = NoteNewRequest("note.template");
      JAddStringToObject(req, "file", "s.qo");
      J *body = JAddObjectToObject(req, "body");
      JAddNumberToObject(body, "temperature", 14.1);
      JAddNumberToObject(body, "humidity", 11);
      NoteRequest(req); }
    { J *rsp = NoteRequestResponse(NoteNewRequest("note.get"));
      if (rsp) { J *body = JGetObject(rsp, "body");
                  if (body) { double t = JGetNumber(body, "temperature");
                              int h = JGetInt(body, "humidity");
                              (void)t; (void)h; }
                  NoteDeleteResponse(rsp); } }
    return 0;
}
EOF

# ── Benchmark: caller function sizes ──────────────────────────────────────────
cat > "$TMPDIR/cpp_callers.cpp" << 'EOF'
#include "mock_backend.hpp"
#include <note/api_context.hpp>
#include <note/body.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/card_version.hpp>
#include <note/api/note_add.hpp>
#include <note/api/note_get.hpp>
#include <note/api/note_template.hpp>

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

MockBackend backend;
note::Notecard nc(backend, [](note::string_view, uint32_t) -> note::Result<std::string> {
    return std::string("{}");
});
note::Api api(nc);

void do_hub_set() {
    api.hubSet().set_product("com.example.app").set_mode("periodic").set_outbound(60).execute();
}
int do_card_version(const char **version, const char **device) {
    auto r = api.cardVersion().execute();
    if (!r) return -1;
    (void)r.version; (void)r.device;
    return 0;
}
void do_note_add(float temp, int16_t humidity) {
    Readings r{.temperature = temp, .humidity = humidity};
    api.noteAdd().set_file("sensors.qo").set_body(r).execute();
}
void do_note_template() {
    api.noteTemplate().set().set_file("sensors.qo").set_body(note::template_of<Readings>()).execute();
}
int do_note_get(float *temp, int16_t *humidity) {
    auto result = api.noteGet().query().set_file("data.qi").execute();
    if (!result) return -1;
    auto r = result.body_as<Readings>();
    *temp = r.temperature; *humidity = r.humidity;
    return 0;
}
int main() { return 0; }
EOF

cat > "$TMPDIR/c_callers.c" << 'EOF'
#include "note.h"
#include <stdlib.h>
void do_hub_set(void) {
    J *req = NoteNewRequest("hub.set");
    JAddStringToObject(req, "product", "com.example.app");
    JAddStringToObject(req, "mode", "periodic");
    JAddNumberToObject(req, "outbound", 60);
    NoteRequest(req);
}
int do_card_version(const char **version, const char **device) {
    J *rsp = NoteRequestResponse(NoteNewRequest("card.version"));
    if (!rsp) return -1;
    if (NoteResponseError(rsp)) { NoteDeleteResponse(rsp); return -1; }
    *version = JGetString(rsp, "version");
    *device = JGetString(rsp, "device");
    NoteDeleteResponse(rsp);
    return 0;
}
void do_note_add(double temp, int humidity) {
    J *req = NoteNewRequest("note.add");
    JAddStringToObject(req, "file", "sensors.qo");
    J *body = JAddObjectToObject(req, "body");
    JAddNumberToObject(body, "temperature", temp);
    JAddNumberToObject(body, "humidity", humidity);
    NoteRequest(req);
}
void do_note_template(void) {
    J *req = NoteNewRequest("note.template");
    JAddStringToObject(req, "file", "sensors.qo");
    J *body = JAddObjectToObject(req, "body");
    JAddNumberToObject(body, "temperature", 14.1);
    JAddNumberToObject(body, "humidity", 11);
    NoteRequest(req);
}
int do_note_get(double *temp, int *humidity) {
    J *req = NoteNewRequest("note.get");
    JAddStringToObject(req, "file", "data.qi");
    J *rsp = NoteRequestResponse(req);
    if (!rsp) return -1;
    if (NoteResponseError(rsp)) { NoteDeleteResponse(rsp); return -1; }
    J *body = JGetObject(rsp, "body");
    if (body) { *temp = JGetNumber(body, "temperature");
                *humidity = JGetInt(body, "humidity"); }
    NoteDeleteResponse(rsp);
    return 0;
}
int main() { return 0; }
EOF

# ── Build note-c static library ───────────────────────────────────────────────
build_note_c_lib() {
    if [ ! -d "$NOTE_C" ]; then
        echo "note-c not found at $NOTE_C — skipping note-c benchmarks."
        echo "Set NOTE_C= or use --note-c to specify the path."
        return 1
    fi

    local cflags="$OPT -DHAVE_STRLCPY -DHAVE_STRLCAT"
    # macOS has strlcpy/strlcat; skip n_str.c
    local srcs=""
    for f in "$NOTE_C"/n_*.c; do
        case "$(basename "$f")" in
            n_str.c) ;; # skip on macOS
            *) srcs="$srcs $f" ;;
        esac
    done

    for f in $srcs; do
        $CC $cflags -I"$NOTE_C" -c "$f" -o "$TMPDIR/$(basename "$f" .c).o" 2>/dev/null
    done
    ar rcs "$TMPDIR/libnote_c.a" "$TMPDIR"/n_*.o 2>/dev/null
}

# ── Helpers ───────────────────────────────────────────────────────────────────
stripped_size() {
    local bin="$1"
    strip -o "${bin}.stripped" "$bin" 2>/dev/null
    stat -f%z "${bin}.stripped" 2>/dev/null || stat -c%s "${bin}.stripped" 2>/dev/null
}

compile_cpp() {
    local src="$1" out="$2"
    $CXX $CXXFLAGS $OPT -I "$ROOT/include" -I "$TMPDIR" -o "$out" "$src" 2>/dev/null
}

compile_c() {
    local src="$1" out="$2"
    $CC $OPT -DHAVE_STRLCPY -DHAVE_STRLCAT -I"$NOTE_C" -o "$out" "$src" "$TMPDIR/libnote_c.a" 2>/dev/null
}

# Extract function sizes from an object file, filtering to do_* functions.
# Output: name<tab>size  (one per line, sorted by name)
function_sizes() {
    local obj="$1" lang="$2"  # lang: c or cpp

    if [ "$lang" = "cpp" ]; then
        nm -n -U "$obj" | c++filt
    else
        nm -n -U "$obj"
    fi | grep ' [Tt] ' | python3 -c "
import sys, re
lines = sys.stdin.readlines()
entries = []
for l in lines:
    parts = l.strip().split(None, 2)
    if len(parts) >= 3 and parts[1] in ('T', 't'):
        entries.append((int(parts[0], 16), parts[2]))
results = {}
for i in range(len(entries)-1):
    addr, name = entries[i]
    size = entries[i+1][0] - addr
    # Strip leading underscore (macOS) and C++ signature
    clean = re.sub(r'^\\_', '', name)
    clean = re.sub(r'\\(.*', '', clean)
    if clean.startswith('do_'):
        results[clean] = results.get(clean, 0) + size
for name in sorted(results):
    print(f'{name}\t{results[name]}')
"
}

# ── Build everything ──────────────────────────────────────────────────────────
HAS_NOTE_C=1
build_note_c_lib || HAS_NOTE_C=0

# note-cpp binaries
for name in minimal 5api allapi body; do
    compile_cpp "$TMPDIR/cpp_${name}.cpp" "$TMPDIR/cpp_${name}"
done

# note-c binaries
if [ "$HAS_NOTE_C" = 1 ]; then
    for name in 5api body; do
        compile_c "$TMPDIR/c_${name}.c" "$TMPDIR/c_${name}"
    done
fi

# Caller object files (for per-function analysis)
$CXX $CXXFLAGS $OPT -I "$ROOT/include" -I "$TMPDIR" -c "$TMPDIR/cpp_callers.cpp" -o "$TMPDIR/cpp_callers.o" 2>/dev/null
if [ "$HAS_NOTE_C" = 1 ]; then
    $CC $OPT -DHAVE_STRLCPY -DHAVE_STRLCAT -I"$NOTE_C" -c "$TMPDIR/c_callers.c" -o "$TMPDIR/c_callers.o" 2>/dev/null
fi

# ── Report: binary sizes ─────────────────────────────────────────────────────
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Code Size Report                                          ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo

echo "── note-cpp stripped binary sizes ($OPT) ────────────────────"
printf "  %-20s %8s  %s\n" "Scenario" "Size" "Description"
printf "  %-20s %8s  %s\n" "--------" "----" "-----------"
for name in minimal 5api allapi body; do
    sz=$(stripped_size "$TMPDIR/cpp_${name}")
    case "$name" in
        minimal) desc="Notecard + 1 ad-hoc request" ;;
        5api)    desc="5 generated API types used" ;;
        allapi)  desc="All 74 API types included, 2 used" ;;
        body)    desc="Body tiers + schema + template_of" ;;
    esac
    printf "  %-20s %6d B  %s\n" "$name" "$sz" "$desc"
done

if [ "$HAS_NOTE_C" = 1 ]; then
    echo
    echo "── note-c stripped binary sizes ($OPT) ──────────────────────"
    printf "  %-20s %8s  %s\n" "Scenario" "Size" "Description"
    printf "  %-20s %8s  %s\n" "--------" "----" "-----------"
    for name in 5api body; do
        sz=$(stripped_size "$TMPDIR/c_${name}")
        printf "  %-20s %6d B  %s\n" "$name" "$sz"
    done

    echo
    echo "── Binary size comparison ($OPT) ────────────────────────────"
    printf "  %-14s %10s %10s %10s\n" "" "note-c" "note-cpp" "delta"
    printf "  %-14s %10s %10s %10s\n" "" "------" "--------" "-----"
    for name in 5api body; do
        c_sz=$(stripped_size "$TMPDIR/c_${name}")
        cpp_sz=$(stripped_size "$TMPDIR/cpp_${name}")
        delta=$((cpp_sz - c_sz))
        pct=$((delta * 100 / c_sz))
        printf "  %-14s %8d B %8d B %+7d B (%+d%%)\n" "$name" "$c_sz" "$cpp_sz" "$delta" "$pct"
    done
    echo
    echo "  note-c links the full library (cJSON, serial, I2C, COBS, MD5, ...)"
    echo "  note-cpp only emits code for templates actually instantiated"
    echo "  Both use mock/null transport"
fi

# ── Report: per-function call-site sizes ──────────────────────────────────────
echo
echo "── Call-site code size ($OPT) ───────────────────────────────"
echo "  Size of each calling function in the application, not the library."
echo

FUNCTIONS="do_hub_set do_card_version do_note_add do_note_template do_note_get"

if [ "$HAS_NOTE_C" = 1 ]; then
    # Collect sizes into temp files for joining
    function_sizes "$TMPDIR/c_callers.o" c > "$TMPDIR/c_fn_sizes.tsv"
    function_sizes "$TMPDIR/cpp_callers.o" cpp > "$TMPDIR/cpp_fn_sizes.tsv"

    python3 - "$TMPDIR/c_fn_sizes.tsv" "$TMPDIR/cpp_fn_sizes.tsv" << 'PYEOF'
import sys

c_file, cpp_file = sys.argv[1], sys.argv[2]

def load(path):
    d = {}
    for line in open(path):
        name, sz = line.strip().split('\t')
        d[name] = int(sz)
    return d

c = load(c_file)
cpp = load(cpp_file)
fns = ["do_hub_set", "do_card_version", "do_note_add", "do_note_template", "do_note_get"]

print(f"  {'Function':<20s} {'note-c':>8s} {'note-cpp':>8s} {'delta':>8s}")
print(f"  {'--------':<20s} {'------':>8s} {'--------':>8s} {'-----':>8s}")

c_total, cpp_total = 0, 0
for fn in fns:
    c_sz = c.get(fn, 0)
    cpp_sz = cpp.get(fn, 0)
    delta = cpp_sz - c_sz
    c_total += c_sz
    cpp_total += cpp_sz
    print(f"  {fn:<20s} {c_sz:>6d} B {cpp_sz:>6d} B {delta:>+6d} B")

print()
delta = cpp_total - c_total
avg = delta // len(fns) if fns else 0
pct = delta * 100 // c_total if c_total else 0
print(f"  {'TOTAL':<20s} {c_total:>6d} B {cpp_total:>6d} B {delta:>+6d} B ({pct:+d}%)")
print(f"  Average overhead per call: {avg:+d} bytes")
PYEOF
else
    echo "  (note-c not available — showing note-cpp only)"
    function_sizes "$TMPDIR/cpp_callers.o" cpp | while IFS=$'\t' read -r name sz; do
        printf "  %-30s %6d B\n" "$name" "$sz"
    done
fi

echo
echo "── What the overhead buys ───────────────────────────────────"
echo "  - Compile-time field name checking (typos won't compile)"
echo "  - Compile-time type checking (wrong types won't compile)"
echo "  - Compile-time enum validation"
echo "  - IDE auto-completion on every field"
echo "  - Schema structs: same type for send, receive, and templates"
echo "  - Response callers can be SMALLER (typed struct vs manual get)"
echo
