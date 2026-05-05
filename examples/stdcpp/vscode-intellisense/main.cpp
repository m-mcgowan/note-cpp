// note-cpp IntelliSense / auto-completion demo
// =============================================
//
// Open this directory as a VS Code workspace
// (File → Open Folder → examples/stdcpp/vscode-intellisense).
// Then run, in a terminal:
//
//     cmake -B build
//
// That generates `build/compile_commands.json`, which both clangd
// (recommended) and the Microsoft C/C++ extension will pick up
// automatically thanks to the `.vscode/settings.json` shipped here.
//
// The demo doesn't talk to a real Notecard. It only exists for the
// editor — `setup()` is enough to exercise the typed API surface.
//
// What to try:
//
//   1. Type-completion on resource groups
//      Place the cursor after `api.` on the `Auto-complete spot 1` line.
//      Ctrl+Space (or trigger completion) — IntelliSense should list
//      hub, card, file, note, env, dfu, var, web, ntn.
//
//   2. Type-completion on endpoint methods
//      After `api.hub.` IntelliSense should offer set/get/sync/signal/
//      log/status/syncStatus.
//
//   3. Type-completion on fluent setters
//      After `.set().` IntelliSense should list every field on
//      api::HubSet — product, mode, outbound, inbound, sn, ...
//
//   4. Inline doc on hover
//      Hover over `mode` or `outbound` — the doc-comment from the
//      Notecard API spec appears, including @since version notes
//      and supported SKUs.
//
//   5. Direct field assignment (typed)
//      The `req.` block below shows the same call as direct assignment.
//      Try Ctrl+Space after `req.` — same field set, same docs.
//
//   6. Body struct
//      Inside the Readings struct below, the fields are completable
//      from the body sender too — see `add().body(readings)`.
//
//   7. Response fields
//      After `.execute();` the result has a typed `.error()`, plus
//      response-specific fields (e.g. `version_rsp.version` on
//      card.version). Hover for docs.

#include <note/notecard.hpp>
#include <note/notecard_api.hpp>
#include <note/units.hpp>

using namespace note::literals;     // 60_mins, 7_days, etc.

// A body struct, defined once. Used below for both sending and
// (in real code) receiving notes with the same shape.
struct Readings {
    float    temperature;
    int16_t  humidity;
    NOTE_FIELDS(temperature, humidity)   // optional on C++20
};

// `setup()` is just a vehicle for IntelliSense — none of these calls
// actually run anywhere in this demo. The Notecard isn't wired to a
// transport here; the goal is purely to make the editor see the API.
void setup(note::NotecardApi<>& api) {
    // ── Auto-complete spot 1 ──────────────────────────────────────────
    // Cursor right after `api.` and trigger completion (Ctrl+Space).
    // Expected: hub, card, file, note, env, dfu, var, web, ntn.

    // ── Auto-complete spot 2 — fluent setters with hover docs ─────────
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)
        .inbound(7_days)
        .execute();

    // ── Auto-complete spot 3 — direct field assignment ────────────────
    auto req = api.hub.set();
    req.product  = "com.example.app";
    req.mode     = "periodic";
    req.outbound = 60_mins;
    req.execute();

    // ── Auto-complete spot 4 — body struct round-trip ─────────────────
    Readings readings{.temperature = 22.5f, .humidity = 60};

    api.note.add()
        .file("sensors.qo")
        .body(readings)             // hover `body`, then `readings`
        .execute();

    // ── Auto-complete spot 5 — typed response fields ─────────────────
    auto rsp = api.card.version().execute();
    if (rsp) {
        // Cursor after `rsp.` for response-field completion.
        // (`device`, `version`, `name`, `sku`, `board`, …)
        (void)rsp.version;
        (void)rsp.device;
    } else {
        (void)rsp.error();
    }
}

int main() {
    // The demo is editor-only — main is here so cmake has something
    // to link. Replace with a real transport (e.g. note::posix::Serial)
    // if you want to actually run it against hardware.
    note::NotecardApi<> api;
    setup(api);
    return 0;
}
