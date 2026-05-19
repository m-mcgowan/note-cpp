// Streaming vs tree mode — body() access, body_or_error(), portable code.
//
// note-cpp can parse Notecard responses two ways:
//
//   * Streaming mode — a SAX parser fires events directly into your typed
//     Response (and into your own struct via .into(T&)). Nothing is held
//     in memory after the call. response.body() returns nullptr because
//     there is no tree to walk.
//
//   * Tree mode — a JsonBackend builds a walkable JsonReader and parks it
//     on the response. response.body() returns the body sub-object so you
//     can walk it by key after the call returns.
//
// The mode is decided once, at Notecard construction:
//
//     Notecard nc(transport);                    // streaming — no backend
//     Notecard nc(backend, transport);           // tree
//
// The typed API surface, .into(T&), .body(struct), nc.transact() etc.
// behave identically in either mode. The handful of surfaces that differ
// are documented in docs/streaming-and-tree.md; this example demonstrates
// the safe-access pattern for code that should run in either.
//
// Build & run (mock canned response, no hardware):
//   c++ -std=c++20 -I include examples/stdcpp/streaming-and-tree.cpp && ./a.out
//
// Run-time output shows the same demo() body executing against three
// configurations so the user can see the surface-level difference.

#include <note/note.hpp>
#include <note/backends/buffer.hpp>   // StaticJsonBackend (zero-heap tree mode)

#include "streaming_mock.hpp"

#include <cstdio>
#include <string>

using namespace note;

// The body shape sensors.qi will carry — same struct used for streaming
// .into(struct) and for tree-mode body()->get_double(...) lookups.
struct Readings {
    float temperature;
    int   humidity;
    NOTE_FIELDS(temperature, humidity)
};

// ── Mock transport: returns a canned note.get response with a body ─────────
class NoteGetMockHal : public StreamingMockHal {
protected:
    void choose_response(const std::string& req) override {
        if (req.find("note.get") != std::string::npos) {
            prime(R"({"body":{"temperature":22.5,"humidity":60},"time":1700000000})");
        } else {
            prime("{}");
        }
    }
};

// ── Pattern 1: explicit tree-mode access via body() ─────────────────────────
// Compiles only against a tree-mode Notecard — body() returns a walkable
// JsonReader pointer. In streaming mode body() always returns nullptr, so
// this pattern would silently produce zero values.
template<typename ApiT>
void demo_tree_body(ApiT& api) {
    std::puts("\n--- tree mode: r.body()->get_double(...) ---");
    auto r = api.note.read("sensors.qi").execute();
    if (!r) {
        std::fprintf(stderr, "  error: %s\n", to_string(r.error()).c_str());
        return;
    }
    if (const JsonReader* body = r.body()) {
        std::printf("  temperature=%.1f humidity=%d (walked from JsonReader)\n",
                    body->get_double("temperature", 0.0),
                    static_cast<int>(body->get_int("humidity", 0)));
    } else {
        std::puts("  body() was null — running in streaming mode? response had no body?");
    }
}

// ── Pattern 2: streaming-friendly access via .into(struct) ──────────────────
// Works identically in both modes. Always prefer this over body() when the
// body shape is known at compile time — it's smaller, faster, and portable.
template<typename ApiT>
void demo_into_struct(ApiT& api) {
    std::puts("\n--- portable: req.into(struct).execute() ---");
    Readings data{};
    auto r = api.note.read("sensors.qi").into(data).execute();
    if (!r) {
        std::fprintf(stderr, "  error: %s\n", to_string(r.error()).c_str());
        return;
    }
    std::printf("  temperature=%.1f humidity=%d (parsed into Readings)\n",
                static_cast<double>(data.temperature), data.humidity);
}

// ── Pattern 3: portable body access via body_or_error() ─────────────────────
// Use when the same code must run against both modes (a shared library, a
// portable provider class), and the body shape is dynamic so .into(struct)
// doesn't apply.
//
// body_or_error() returns:
//   * a (possibly null) JsonReader* in tree mode — null means the response
//     carried no "body" field, the response itself succeeded
//   * an Error::NotReady with an actionable message in streaming mode
template<typename ApiT>
void demo_body_or_error(ApiT& api) {
    std::puts("\n--- portable: r.body_or_error() handles both modes ---");
    auto r = api.note.read("sensors.qi").execute();
    if (!r) {
        std::fprintf(stderr, "  error: %s\n", to_string(r.error()).c_str());
        return;
    }
    if (r.was_streaming_parse()) {
        std::puts("  streaming mode — no tree to walk; "
                  "use .into(T&) or .into(JsonSink&) for body access");
    }
    auto body_result = r.body_or_error();
    if (body_result.has_value()) {
        const JsonReader* body = *body_result;
        if (body) {
            std::printf("  temperature=%.1f humidity=%d (tree path)\n",
                        body->get_double("temperature", 0.0),
                        static_cast<int>(body->get_int("humidity", 0)));
        } else {
            std::puts("  tree mode but response carried no body field");
        }
    } else {
        // Error::NotReady — running in streaming mode. The message names
        // .into() as the alternative so callers can surface it to users.
        std::printf("  body unavailable: %s\n",
                    to_string(body_result.error()).c_str());
    }
}

int main() {
    NoteGetMockHal hal;
    Protocol       transport{hal};

    // ── Run 1: streaming mode (no JsonBackend linked) ──────────────────────
    std::puts("=== Streaming mode (no JsonBackend) ===");
    {
        Notecard nc(transport);          // new convenience ctor — no arena needed
        Api      api(nc);
        demo_tree_body(api);             // body() returns null
        demo_into_struct(api);           // .into(struct) works
        demo_body_or_error(api);         // body_or_error() returns Error::NotReady
    }

    // ── Run 2: tree mode (StaticJsonBackend — zero heap, real parse) ───────
    std::puts("\n=== Tree mode (StaticJsonBackend, zero heap) ===");
    {
        backends::StaticJsonBackend<512, 64> backend;
        Notecard nc(backend, transport);
        Api      api(nc);
        demo_tree_body(api);             // body() returns a walkable reader
        demo_into_struct(api);           // .into(struct) also works
        demo_body_or_error(api);         // body_or_error() returns success
    }

    std::puts("\nAll patterns demonstrated.");
    return 0;
}
