// Arduino migration example — compiled with PlatformIO to verify every
// code snippet in docs/migration-from-note-arduino.md works on real Arduino.
//
// This file is the source of truth for the migration guide's note-cpp
// examples. The doc references line numbers from this file via embedme.

#include <note/arduino.hpp>
using namespace note::api;
using namespace note::literals;


// ── Body struct (used across note.add, templates, and receive examples) ──

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};


// ── Setup ────────────────────────────────────────────────────────────────

note::arduino::Notecard nc;

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


// ── hub.set: designated initializers ─────────────────────────────────────

void hub_set_desig() {
    HubSet req{
        .mode     = "periodic",
        .outbound = 60_mins,
        .product  = "com.example.app",
    };
    nc.execute(req);
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
        auto err = result.error();
        Serial.println(err);
    }
}


// ── note.template: register for compact storage ──────────────────────────

void note_template() {
    nc.note.templates().define("sensors.qo")
        .body(note::template_of(Readings{}))
        .execute();
}


// ── card.temp: read temperature ──────────────────────────────────────────

void card_temp() {
    auto r = nc.card.temp().read().execute();
    if (r) {
        double temp = r.value;
        Serial.println(temp);
    } else {
        Serial.println(r.error());
    }
}


// ── card.version: read device info ───────────────────────────────────────

void card_version() {
    auto r = nc.card.version().execute();
    if (r) {
        auto ver = r.version;
        auto dev = r.device;
        Serial.println(ver);
        Serial.println(dev);
    }
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

void card_attn_intent() {
    nc.execute(CardAttn::Arm{}.connected());
    nc.execute(CardAttn::Disarm{});
}


// ── card.attn: sleep with state ──────────────────────────────────────────

void card_attn_sleep() {
    CardAttn::Sleep req;
    req.seconds(1_hours);
    req.payload("checkpoint-v1");
    nc.execute(req);
}

void card_attn_retrieve() {
    auto r = nc.execute(CardAttn::Retrieve{});
    if (r && r.time != 0) {
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
        auto err = r.error();
        Serial.println(err);
    }
}


// ── fire-and-forget command ──────────────────────────────────────────────

void hub_sync() {
    nc.hub.sync().command();
}


void loop() {
    // Examples are called from setup or on demand — loop is empty.
}
