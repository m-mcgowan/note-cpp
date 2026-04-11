// Arduino migration example — compiled with PlatformIO to verify every
// code snippet in docs/guides/migration-from-note-arduino.md works on
// real Arduino.
//
// This file is the source of truth for the migration guide's note-cpp
// examples. The doc references line numbers from this file via embedme.
//
// IMPORTANT: If you edit this file, update the line number references in:
//   - docs/guides/migration-from-note-arduino.md
//   - README.md (if the same snippet appears there)
// Run tools/verify-docs.sh to check for mismatches.

#include <note.hpp>


// ── Body struct (used across note.add, templates, and receive examples) ──

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // not needed on C++20
};


// ── Setup ────────────────────────────────────────────────────────────────

Notecard nc;

void setup() {
    nc.begin(Serial1, 9600);
}


// ── hub.set: fluent chain ────────────────────────────────────────────────

void hub_set_fluent() {
    nc.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)
        .execute();
}


// ── hub.set: direct assignment with conditional logic ────────────────────

void hub_set_direct() {
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


// ── note.add: send sensor data ───────────────────────────────────────────

void note_add() {
    Readings r{.temperature = 22.5f, .humidity = 60};
    nc.note.add()
        .file("sensors.qo")
        .body(r)
        .execute();
}


// ── note.add: with error handling ────────────────────────────────────────

void note_add_errors() {
    Readings r{.temperature = 22.5f, .humidity = 60};
    auto result = nc.note.add()
        .file("sensors.qo")
        .body(r)
        .execute();
    if (!result) {
        Serial.println(result.error());
    }
}


// ── note.template: register for compact storage ──────────────────────────

void note_template() {
    nc.note.templates().define("sensors.qo")
        .body(note::template_of(Readings()))
        .execute();
}


// ── card.temp: read temperature ──────────────────────────────────────────

void card_temp() {
    auto r = nc.card.temp().read().execute();
    if (r) {
        Serial.println(r.value);
    } else {
        Serial.println(r.error());
    }
}

// Configure periodic monitoring
void card_temp_configure() {
    nc.card.temp().configure()
        .minutes(5)
        .execute();
}


// ── card.version: read device info ───────────────────────────────────────

void card_version() {
    auto r = nc.card.version().execute();
    if (r) {
        Serial.println(r.version);
        Serial.println(r.device);
    }
}


// ── Response lifetime ────────────────────────────────────────────────────
// Non-string fields (int, bool, double) are plain values — always safe.
// String fields (string_view) are valid until the next execute().
// See docs/response-lifetimes.md for arena-based string extension.

double last_temperature = 0;

void keep_response() {
    auto r = nc.card.temp().read().execute();
    if (r) {
        last_temperature = r.value;  // safe — plain double
    }
    // Response goes out of scope here — no manual cleanup needed.
}


// ── card.attn: arm for interrupts ────────────────────────────────────────

void card_attn_arm() {
    nc.card.attn().arm()
        .connected()
        .motion()
        .seconds(120_s)
        .execute();
}

void card_attn_disarm() {
    nc.card.attn().disarm().execute();
}


// ── card.attn: sleep with state ──────────────────────────────────────────

void card_attn_sleep() {
    auto req = nc.card.attn().sleep();
    req.seconds = 1_hours;
    req.payload = "checkpoint-v1";
    req.execute();
}

void card_attn_retrieve() {
    auto r = nc.card.attn().retrieve().execute();
    if (r && r.time.has_value()) {
        auto payload = r.payload;
        (void)payload;
    }
}


// ── env.get: read environment variable ───────────────────────────────────

void env_get() {
    auto r = nc.env.get()
        .name("interval")
        .execute();
    if (r) {
        auto text = r.text;
        (void)text;
    }
}

void env_set_default() {
    nc.env.setDefault("interval", "60").execute();
}


// ── error handling ───────────────────────────────────────────────────────

void error_handling() {
    auto r = nc.card.version().execute();
    if (!r) {
        Serial.println(r.error());
    }
}

void error_details() {
    auto r = nc.card.version().execute();
    if (!r) {
        auto err = r.error();
        // err.code:    Error::Notecard, SendFailed, etc.
        // err.cause:   Cause::Timeout, HalError, etc.
        // err.message: human-readable string
        Serial.println(err);
    } else {
        // use r.version, r.device, etc.
    }
}


// ── fire-and-forget command ──────────────────────────────────────────────

void hub_sync() {
    nc.hub.sync().command();
}


void loop() {}
