// Environment variables with env.get — all the ways to use it.
//
// The Notecard exposes a key/value environment-variable store. Values
// come from Notecard local defaults (env.default), the host (env.set),
// or Notehub (synced from the project). env.get is how the firmware
// reads them. Four modes:
//
//   1. One variable   → response.text is the value.
//   2. Many variables → response body is an object; stream into a struct.
//   3. All variables  → same as (2) but without a names list.
//   4. Conditional    → .time(t) returns results only if changed since t.
//
// String lifetime: DeviceConfig.region / .locale are declared as
// note::string_view, so they follow the Notecard's allocator rule —
// heap-interned by default (valid for the lifetime of the response),
// or arena-interned if you set an arena. For values you want to copy
// into struct-owned storage instead, declare them as std::string —
// see docs/response-lifetimes.md for the full story.
//
// Build & run (mock, no hardware):
//   c++ -std=c++20 -I include examples/stdcpp/env-vars.cpp && ./a.out
//
// Build & run (real Notecard over USB serial):
//   c++ -std=c++20 -I include examples/stdcpp/env-vars.cpp -o env-vars
//   ./env-vars /dev/cu.usbmodemNOTE1        # macOS
//   ./env-vars /dev/ttyUSB0                 # Linux
//
// With no device-path arg, the example uses a streaming mock with
// canned responses so CI can build + run it without hardware.

#include <note/note.hpp>       // umbrella: typed API + compile-time JSON helpers

#include "streaming_mock.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <note/posix.hpp>
#endif

#include <cstdio>
#include <cstring>
#include <string>

using namespace note;

// Fields map one-to-one to env-var names on the Notecard. StructSink
// matches body-object keys to struct fields by name. Any integer or
// float type works — StructSink narrows from the wire integer to
// whatever your field is.
//
// string_view fields are cheap; their lifetime tracks the allocator
// (the Response's heap-interned storage by default, or your arena if
// you set one). std::string fields copy into the struct itself and
// outlive the Response unconditionally. Pick per field.
struct DeviceConfig {
    note::string_view region;
    note::string_view locale;
    int               interval;
    NOTE_FIELDS(region, locale, interval)
};

// ─── Streaming mock for the no-hardware path ─────────────────────────────
class EnvVarsMockHal : public StreamingMockHal {
protected:
    void choose_response(const std::string& req) override {
        if (req.find(R"("name":"region")") != std::string::npos)
            prime(R"({"text":"us-east-1","time":1700000000})");
        else if (req.find(R"("names":)") != std::string::npos)
            prime(R"({"body":{"region":"us-east-1","locale":"en-US","interval":300},"time":1700000000})");
        else if (req.find("env.modified") != std::string::npos)
            prime(R"({"time":1700000000})");
        else if (req.find("env.get") != std::string::npos)
            prime(R"({"body":{"region":"us-east-1","locale":"en-US","interval":300,"debug":"false"},"time":1700000000})");
        else
            prime("{}");
    }
};

// Demo that works against any Notecard API wrapper — mock or real.
template<typename NotecardT>
void demo(NotecardT& nc) {
    // Api<> exposes the typed request factories. For note::Notecard directly.
    // For note::posix::Notecard it's already mixed in via NotecardApi.
    Api api(nc);
    run_demo_on(api);
}

template<typename ApiT>
void run_demo_on(ApiT& api) {
    // ── 1. Single variable ────────────────────────────────────────────────
    std::puts("\n--- single: env.get(\"region\") ---");
    {
        auto r = api.env.get().name("region").execute();
        if (r) {
            std::printf("  region = %.*s (time=%lld)\n",
                        (int)r.text.value().size(), r.text.value().data(),
                        static_cast<long long>(r.time));
        } else {
            std::fprintf(stderr, "  error: %s\n",
                         note::to_string(r.error()).c_str());
        }
    }

    // ── 2. Multiple variables → struct ────────────────────────────────────
    // `.names({...})` returns the ArrayField (not the request), so set
    // names on the request object directly, then chain .into/.execute.
    std::puts("\n--- multi: env.get + names, .into(cfg) ---");
    {
        DeviceConfig cfg{};
        auto req = api.env.get();
        req.names = {"region", "locale", "interval"};
        auto r = req.into(cfg).execute();
        if (r) {
            std::printf("  region=%.*s locale=%.*s interval=%d\n",
                        (int)cfg.region.size(), cfg.region.data(),
                        (int)cfg.locale.size(), cfg.locale.data(),
                        cfg.interval);
        }
    }

    // ── 3. All variables → struct ─────────────────────────────────────────
    std::puts("\n--- all: env.get().into(cfg) ---");
    {
        DeviceConfig cfg{};
        auto r = api.env.get().into(cfg).execute();
        if (r) {
            std::printf("  region=%.*s locale=%.*s interval=%d\n",
                        (int)cfg.region.size(), cfg.region.data(),
                        (int)cfg.locale.size(), cfg.locale.data(),
                        cfg.interval);
        }
    }

    // ── 4. Conditional: only if changed since last poll ───────────────────
    std::puts("\n--- conditional: .time(last_seen) + into(cfg) ---");
    {
        DeviceConfig cfg{};
        note::json_int_t last_seen = 1699999999;
        auto req = api.env.get();
        req.names = {"region", "locale", "interval"};
        req.time = last_seen;
        auto r = req.into(cfg).execute();
        if (r) {
            if (static_cast<long long>(r.time) > last_seen) {
                std::printf("  changed — region=%.*s interval=%d\n",
                            (int)cfg.region.size(), cfg.region.data(),
                            cfg.interval);
            } else {
                std::puts("  no change since last poll");
            }
        }
    }

    // ── 5. All variables, dynamic keys → JsonSink ─────────────────────────
    // For `env.get` with no `name`/`names`, the response body's keys are
    // whatever Notehub has provisioned — not known at compile time. `.into(struct)`
    // can't match keys it doesn't have. The streaming-friendly answer is a
    // `JsonSink`: a tiny subclass that receives one callback per body field,
    // so you can dispatch by key at runtime.
    //
    // This is the streaming-mode counterpart to walking a tree via
    // `r.body()`. Works on every platform — no `JsonBackend` needed, no
    // staging buffer to size.
    std::puts("\n--- dynamic keys: env.get + .into(JsonSink&) ---");
    {
        // The `.into(JsonSink&)` overload wires your sink to body events
        // only — top-level Response fields like `time` go through the
        // typed Response, not through the sink. So every on_* call your
        // sink receives is a body field; no depth tracking needed.
        struct EnvSink : note::JsonSink {
            void on_string(note::string_view k, note::string_view v) override {
                std::printf("  %.*s = %.*s\n",
                            (int)k.size(), k.data(),
                            (int)v.size(), v.data());
            }
            void on_int(note::string_view k, note::json_int_t v) override {
                std::printf("  %.*s = %lld\n",
                            (int)k.size(), k.data(),
                            static_cast<long long>(v));
            }
            // on_bool, on_float etc. left as defaults; extend as needed.
        };
        EnvSink sink;
        auto r = api.env.get().into(sink).execute();
        if (r) {
            std::printf("  (time=%lld)\n", static_cast<long long>(r.time));
        }
    }

    // ── Related: setting variables and cheap change detection ─────────────
    std::puts("\n--- env.default / env.set / env.modified ---");
    api.env.setDefault("interval", "300").execute();
    api.env.set("debug").text("true").execute();
    if (auto r = api.env.modified().execute(); r) {
        std::printf("  env store last modified: %lld\n",
                    static_cast<long long>(r.time));
    }

    // ── Compile-time env.default (zero runtime cost) ──────────────────────
    std::puts("\n--- env.default (compile-time JSON) ---");
    constexpr auto default_interval_json = note::json<[](auto& b) {
        b.add("req", "env.default");
        b.add("name", "interval");
        b.add("text", "300");
        b.close();
    }>();
    static_assert(default_interval_json.view() ==
        R"({"req":"env.default","name":"interval","text":"300"})");
    std::printf("  %.*s\n", (int)default_interval_json.size(),
                default_interval_json.data());
}

int main(int argc, char** argv) {
    if (argc > 1) {
#if defined(__unix__) || defined(__APPLE__)
        // Real Notecard over USB serial. Matches examples/stdcpp/posix-hardware.cpp.
        const char* path = argv[1];
        note::posix::Notecard nc;
        nc.begin(path);
        std::printf("=== real Notecard at %s ===\n", path);
        run_demo_on(static_cast<note::Api<>&>(nc));
#else
        std::fprintf(stderr, "real-hardware mode requires a POSIX host\n");
        return 1;
#endif
    } else {
        // Mock path — CI builds and runs this, no hardware needed.
        EnvVarsMockHal hal;
        note::Protocol transport{hal};
        note::Notecard nc(transport);
        Api api(nc);
        std::puts("=== mock (streaming canned responses) ===");
        run_demo_on(api);
    }

    std::puts("\nAll env.get patterns demonstrated.");
    return 0;
}
