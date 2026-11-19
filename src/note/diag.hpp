#pragma once
/// @file diag.hpp
/// Lightweight diagnostics for API-misuse that can't be caught at compile time.
///
/// Currently: reporting a call to `Response::body()` in streaming mode when a
/// body was present on the wire but not captured (so `body()` is null). The
/// streaming-vs-tree choice is made at runtime (`set_allocator`), so this can't
/// be a compile error in a tree-capable build; this makes the silent null loud.
///
/// Default behaviour is a log (soft), so it "at least makes the issue clear"
/// without disrupting a running app. Define `NOTE_STREAMING_BODY_STRICT=1`
/// (CI/test builds) to abort instead, so the misuse fails a test run.

#include <note/note_config.hpp>

#ifndef NOTE_STREAMING_BODY_STRICT
#define NOTE_STREAMING_BODY_STRICT 0
#endif

#if !NOTE_NO_STD_STRING || NOTE_STREAMING_BODY_STRICT
#include <cstdio>
#endif
#if NOTE_STREAMING_BODY_STRICT
#include <cstdlib>
#endif

namespace note {
namespace detail {

/// Optional hook to route diagnostics to an app's debug output (e.g.
/// `Serial.println`). When null, the soft path logs to stderr on hosts and is
/// silent on embedded targets without stdio. Set it to see warnings on a
/// microcontroller:
///   note::detail::streaming_body_warn_hook() = [](const char* m){ Serial.println(m); };
using StreamingBodyWarnFn = void (*)(const char*);
inline StreamingBodyWarnFn& streaming_body_warn_hook() {
    static StreamingBodyWarnFn hook = nullptr;
    return hook;
}

/// Report misuse of `body()` in streaming mode. Soft (log) by default; aborts
/// when NOTE_STREAMING_BODY_STRICT=1.
[[maybe_unused]] inline void report_streaming_body_misuse() {
    static constexpr char msg[] =
        "note-cpp: body() is null because the response was parsed in streaming "
        "mode and a body was present but not captured. Wire .into(MyStruct&) / "
        ".body(JsonSink&), or use body_or_error().";
    if (auto hook = streaming_body_warn_hook()) {
        hook(msg);
    }
#if !NOTE_NO_STD_STRING
    else {
        std::fprintf(stderr, "%s\n", msg);
    }
#endif
#if NOTE_STREAMING_BODY_STRICT
    std::abort();
#endif
}

// ── Compile-time error carriers for generated API validators ────────────────
// The generated consteval validators (enum-value and flag checks) report an
// invalid compile-time literal by calling one of these undefined, non-constexpr
// functions. Calling a non-constexpr function during constant evaluation is
// ill-formed, so the check fails to compile — WITHOUT a `throw`, so the headers
// build under -fno-exceptions (the common exception-free embedded config). The
// compile error points at the exact generated validator; these are never
// defined and never odr-used at runtime.
void note_api_invalid_field_value();
void note_api_invalid_flag();

} // namespace detail
} // namespace note
