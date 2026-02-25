// Getting started with note-cpp: common Notecard operations.
//
// Each example shows the type-safe C++ equivalent of the JSON requests
// from the Blues Notecard API documentation:
//   https://dev.blues.io/api-reference/notecard-api/introduction/
//
// Build: c++ -std=c++2b -I include examples/getting_started.cpp

#include <note/api_context.hpp>
#include <note/body.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/hub_sync.hpp>
#include <note/api/hub_status.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_status.hpp>
#include <note/api/card_wireless.hpp>
#include <note/api/note_add.hpp>
#include <note/api/note_get.hpp>
#include <note/api/note_template.hpp>
#include <note/api/env_set.hpp>
#include <note/api/env_get.hpp>

// Schema structs — define once, use for send, receive, and templates.
// C++20: plain aggregates work automatically via reflection.
// C++17: use NOTE_BODY() macro for the same functionality.

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_BODY(temperature, humidity)
};

// Users provide their own JsonBackend and transport callable implementations.
// See examples/smoke.cpp for mock implementations.
void examples(note::Notecard& nc) {
    // Create an Api instance bound to this notecard.
    note::Api api(nc);

    // -----------------------------------------------------------------------
    // Product configuration
    // https://dev.blues.io/api-reference/notecard-api/hub-requests/#hub-set
    // -----------------------------------------------------------------------

    // Fluent factory chain:
    // {"req":"hub.set","mode":"periodic","outbound":60,"product":"com.example.app"}
    api.hubSet()
       .set_product("com.example.app")
       .set_mode("periodic")
       .set_outbound(60)
       .execute();

    // Direct field assignment:
    // {"req":"hub.set","mode":"continuous"}
    {
        auto req = api.hubSet();
        req.mode = "continuous";
        req.execute();
    }

    // Designated initializers (fields in declaration order):
    // {"req":"hub.set","mode":"minimum","product":"com.example.app"}
    api.execute(note::api::HubSet{.mode = "minimum", .product = "com.example.app"});

    // Compile-time validated enum values:
    {
        auto req = api.hubSet();
        req.mode = note::api::HubSet::validated_mode("periodic");  // OK
        // req.mode = note::api::HubSet::validated_mode("typo");   // COMPILE ERROR
        req.execute();
    }

    // -----------------------------------------------------------------------
    // Syncing
    // https://dev.blues.io/api-reference/notecard-api/hub-requests/#hub-sync
    // -----------------------------------------------------------------------

    // {"req":"hub.sync"}
    api.hubSync().execute();

    // -----------------------------------------------------------------------
    // Device info
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-version
    // -----------------------------------------------------------------------

    // {"req":"card.version"}
    {
        auto result = api.cardVersion().execute();
        if (result) {
            note::string_view version = result.version;
            note::string_view device = result.device;
            (void)version;
            (void)device;
        }
    }

    // -----------------------------------------------------------------------
    // Device status
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-status
    // -----------------------------------------------------------------------

    // {"req":"card.status"}
    {
        auto result = api.cardStatus().execute();
        if (result) {
            auto usb = result.usb;
            auto storage = result.storage;
            (void)usb;
            (void)storage;
        }
    }

    // -----------------------------------------------------------------------
    // Adding Notes — three body tiers
    // https://dev.blues.io/api-reference/notecard-api/note-requests/#note-add
    // -----------------------------------------------------------------------

    // Tier 1: Raw JSON string body (works everywhere)
    // {"req":"note.add","body":"{\"temp\":22.5}","file":"sensors.qo"}
    api.noteAdd().set_file("sensors.qo").set_body(R"({"temp":22.5})").execute();

    // Tier 2: Builder lambda (structured body, no schema)
    // {"req":"note.add","body":{"temp":22.5,"humidity":60},"file":"sensors.qo"}
    api.noteAdd()
        .set_file("sensors.qo")
        .set_body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("humidity", int32_t{60});
        }))
        .execute();

    // Tier 3: Schema struct — same Readings type for send, receive, and templates.
    // NOTE_BODY macro works on C++17; on C++20 it's optional (reflection does it).
    // {"req":"note.add","body":{"temperature":22.5,"humidity":60},"file":"sensors.qo"}
    {
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.noteAdd().set_file("sensors.qo").set_body(r).execute();
    }

    // -----------------------------------------------------------------------
    // Note templates — register schemas with type hints
    // https://dev.blues.io/api-reference/notecard-api/note-requests/#note-template
    // -----------------------------------------------------------------------

    // Auto-generate template from C++ struct
    // {"req":"note.template","body":{"temperature":14.1,"humidity":11},"file":"sensors.qo"}
    api.noteTemplate().set()
        .set_file("sensors.qo")
        .set_body(note::template_of<Readings>())
        .execute();

    // -----------------------------------------------------------------------
    // Reading Notes — typed response body
    // https://dev.blues.io/api-reference/notecard-api/note-requests/#note-get
    // -----------------------------------------------------------------------

    // {"req":"note.get","file":"data.qi"} — query without deleting
    {
        auto result = api.noteGet().query().set_file("data.qi").execute();
        if (result) {
            // Access response scalar fields
            auto payload = result.payload;
            auto time = result.time;
            (void)payload;
            (void)time;

            // Access body as raw reader (ad-hoc)
            if (auto* body = result.body()) {
                auto temp = body->get_double("temp");
                (void)temp;
            }

            // Access body as typed struct (C++20 or NOTE_BODY)
            auto r = result.body_as<Readings>();
            (void)r.temperature;
            (void)r.humidity;
        }
    }

    // {"req":"note.get","delete":true,"file":"requests.qi"} — pop from queue
    api.noteGet().delete_().set_file("requests.qi").execute();

    // -----------------------------------------------------------------------
    // Environment variables
    // https://dev.blues.io/api-reference/notecard-api/env-requests/#env-set
    // -----------------------------------------------------------------------

    // Designated initializers:
    // {"req":"env.set","name":"interval","text":"300"}
    api.execute(note::api::EnvSet{.name = "interval", .text = "300"});

    // -----------------------------------------------------------------------
    // Fire-and-forget commands
    // https://dev.blues.io/api-reference/notecard-api/hub-requests/#hub-set
    // -----------------------------------------------------------------------

    // {"cmd":"hub.set","product":"com.example.app"}
    {
        auto req = api.hubSet();
        req.product = "com.example.app";
        req.command();
    }

    // -----------------------------------------------------------------------
    // Standalone usage (without Api factory)
    // -----------------------------------------------------------------------

    // req.execute(nc) — pass notecard explicitly:
    {
        note::api::CardVersion req;
        auto result = req.execute(nc);
        (void)result;
    }

    // nc.execute(req) — original style:
    {
        note::api::HubSet req;
        req.set_product("test");
        nc.execute(req);
    }

    // Ad-hoc request (for anything not yet generated):
    {
        auto result = nc.request("card.version");
        if (result) {
            auto version = (*result)->get_string("version");
            (void)version;
        }
    }
}

int main() {
    // This file is for illustration only — requires real backend and IO
    // implementations to compile and link. See examples/smoke.cpp for
    // a compilable mock example.
    return 0;
}
