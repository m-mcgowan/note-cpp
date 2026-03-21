// migration.cpp — compiled examples for the migration guide.
//
// Every note-cpp snippet in docs/migration-from-note-arduino.md comes from
// this file, verified by CI. The note-c snippets are in migration_notec.c.
//
// Build: c++ -std=c++20 -I include examples/migration.cpp && ./a.out

#include <note/notecard.hpp>
#include <note/notecard_api.hpp>
#include <note/api.hpp>
#include <note/body.hpp>

#include "mock_backend.hpp"
#include <cstdio>

using namespace note::api;
using namespace note::literals;


// ── Body struct (shared across note.add, templates, and receive examples) ──

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};


int main() {
    MockBackend backend;
    note::NotecardApi nc(backend,
        [](note::string_view request, uint32_t) -> note::Result<note::string_view> {
            std::printf("  >> %.*s\n", (int)request.size(), request.data());
            return note::string_view("{}");
        });


    // ── hub.set: fluent chain ───────────────────────────────────────────

    std::puts("\n--- hub.set (fluent) ---");
    nc.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)
        .execute();


    // ── hub.set: direct assignment with conditional logic ───────────────

    std::puts("\n--- hub.set (direct assignment) ---");
    {
        bool use_continuous = false;
        auto req = nc.hub.set();
        req.product  = "com.example.app";
        req.outbound = 60_mins;
        if (use_continuous) {
            req.mode = "continuous";
            req.sync = true;
        } else {
            req.mode = "periodic";
        }
        req.execute();
    }


    // ── hub.set: designated initializers ────────────────────────────────

    std::puts("\n--- hub.set (designated initializers) ---");
    {
        // Fields must be in declaration order (alphabetical in generated types).
        HubSet req{
            .mode     = "periodic",
            .outbound = 60_mins,
            .product  = "com.example.app",
        };
        nc.execute(req);
    }


    // ── note.add: send sensor data ─────────────────────────────────────

    std::puts("\n--- note.add ---");
    {
        Readings r{.temperature = 22.5f, .humidity = 60};
        nc.note.add()
            .file("sensors.qo")
            .body(r)
            .execute();
    }


    // ── note.add: with error handling ──────────────────────────────────

    std::puts("\n--- note.add (error handling) ---");
    {
        Readings r{.temperature = 22.5f, .humidity = 60};
        auto result = nc.note.add()
            .file("sensors.qo")
            .body(r)
            .execute();
        if (!result) {
            auto err = result.error();
            std::printf("  error: %s\n", to_string(err).c_str());

            // err.code — what went wrong:
            //   Error::SendFailed  — never reached the Notecard (safe to retry)
            //   Error::Notecard    — Notecard returned an error
            //   Error::Json        — response couldn't be parsed
            // err.cause — why:
            //   Cause::Timeout, Cause::HalError, Cause::CrcMismatch, etc.
            // err.message — the Notecard's error string:
            //   "note.add: file not found"
            //   "note.add: queue full"
        }
    }


    // ── note.template: register for compact storage ────────────────────

    std::puts("\n--- note.template ---");
    nc.note.templates().define("sensors.qo")
        .body(note::template_of(Readings{}))
        .execute();


    // ── card.temp: read temperature ────────────────────────────────────

    std::puts("\n--- card.temp ---");
    {
        auto r = nc.card.temp().read().execute();
        if (r) {
            double temp = r.value;
            std::printf("  temp: %f\n", temp);
        } else {
            std::printf("  error: %s\n", to_string(r.error()).c_str());
        }
    }


    // ── card.version: read device info ─────────────────────────────────

    std::puts("\n--- card.version ---");
    {
        auto r = nc.card.version().execute();
        if (r) {
            note::string_view ver = r.version;
            note::string_view dev = r.device;
            (void)ver; (void)dev;
        }
    }


    // ── card.attn: arm for interrupts ──────────────────────────────────

    std::puts("\n--- card.attn (arm) ---");
    nc.card.attn().arm()
        .connected()
        .motion()
        .seconds(120_s)
        .execute();

    std::puts("\n--- card.attn (disarm) ---");
    nc.card.attn().disarm().execute();

    // Intent-based types:
    std::puts("\n--- card.attn (intent types) ---");
    nc.execute(CardAttn::Arm{}.connected());
    nc.execute(CardAttn::Disarm{});


    // ── card.attn: sleep with state ────────────────────────────────────

    std::puts("\n--- card.attn (sleep) ---");
    {
        CardAttn::Sleep req;
        req.seconds(1_hours);
        req.payload("checkpoint-v1");
        nc.execute(req);
    }

    std::puts("\n--- card.attn (retrieve) ---");
    {
        auto r = nc.execute(CardAttn::Retrieve{});
        if (r && r.time != 0) {
            note::string_view payload = r.payload;
            (void)payload;
        }
    }


    // ── env.get: read environment variable ─────────────────────────────

    std::puts("\n--- env.get ---");
    {
        auto r = nc.env.get()
            .name("interval")
            .execute();
        if (r) {
            note::string_view text = r.text;
            (void)text;
        }
    }

    std::puts("\n--- env.setDefault ---");
    nc.env.setDefault("interval", "60").execute();


    // ── error handling ─────────────────────────────────────────────────

    std::puts("\n--- error handling ---");
    {
        auto r = nc.card.version().execute();
        if (!r) {
            auto err = r.error();
            std::printf("  error: %s\n", to_string(err).c_str());
        }
    }


    // ── fire-and-forget command ────────────────────────────────────────

    std::puts("\n--- hub.sync (command) ---");
    nc.hub.sync().command();


    std::puts("\nAll migration examples completed.");
    return 0;
}
